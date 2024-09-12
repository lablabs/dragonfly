// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/command_registry.h"

#include <absl/strings/str_split.h>
#include <absl/time/clock.h>

#include "absl/container/inlined_vector.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "base/bits.h"
#include "base/flags.h"
#include "base/logging.h"
#include "facade/dragonfly_connection.h"
#include "facade/error.h"
#include "server/acl/acl_commands_def.h"
#include "server/server_state.h"

using namespace std;
ABSL_FLAG(vector<string>, rename_command, {},
          "Change the name of commands, format is: <cmd1_name>=<cmd1_new_name>, "
          "<cmd2_name>=<cmd2_new_name>");
ABSL_FLAG(vector<string>, restricted_commands, {},
          "Commands restricted to connections on the admin port");

namespace dfly {

using namespace facade;

using absl::AsciiStrToUpper;
using absl::GetFlag;
using absl::StrAppend;
using absl::StrCat;
using absl::StrSplit;

CommandId::CommandId(const char* name, uint32_t mask, int8_t arity, int8_t first_key,
                     int8_t last_key, uint32_t acl_categories)
    : facade::CommandId(name, mask, arity, first_key, last_key, acl_categories) {
  if (mask & CO::ADMIN)
    opt_mask_ |= CO::NOSCRIPT;
}

bool CommandId::IsTransactional() const {
  if (first_key_ > 0 || (opt_mask_ & CO::GLOBAL_TRANS) || (opt_mask_ & CO::NO_KEY_TRANSACTIONAL))
    return true;

  if (name_ == "EVAL" || name_ == "EVALSHA" || name_ == "EXEC")
    return true;

  return false;
}

bool CommandId::IsMultiTransactional() const {
  return CO::IsTransKind(name()) || CO::IsEvalKind(name());
}

uint64_t CommandId::Invoke(CmdArgList args, ConnectionContext* cntx) const {
  int64_t before = absl::GetCurrentTimeNanos();
  handler_(args, cntx);
  int64_t after = absl::GetCurrentTimeNanos();

  ServerState* ss = ServerState::tlocal();  // Might have migrated thread, read after invocation
  int64_t execution_time_usec = (after - before) / 1000;

  auto& ent = command_stats_[ss->thread_index()];

  ++ent.first;
  ent.second += execution_time_usec;

  return execution_time_usec;
}

optional<facade::ErrorReply> CommandId::Validate(CmdArgList tail_args) const {
  if ((arity() > 0 && tail_args.size() + 1 != size_t(arity())) ||
      (arity() < 0 && tail_args.size() + 1 < size_t(-arity()))) {
    string prefix;
    if (name() == "EXEC")
      prefix = "-EXECABORT Transaction discarded because of: ";
    return facade::ErrorReply{prefix + facade::WrongNumArgsError(name()), kSyntaxErrType};
  }

  if ((opt_mask() & CO::INTERLEAVED_KEYS)) {
    if ((name() == "JSON.MSET" && tail_args.size() % 3 != 0) ||
        (name() == "MSET" && tail_args.size() % 2 != 0))
      return facade::ErrorReply{facade::WrongNumArgsError(name()), kSyntaxErrType};
  }

  if (validator_)
    return validator_(tail_args);
  return nullopt;
}

CommandRegistry::CommandRegistry() {
  vector<string> rename_command = GetFlag(FLAGS_rename_command);

  for (string command_data : rename_command) {
    pair<string_view, string_view> kv = StrSplit(command_data, '=');
    auto [_, inserted] =
        cmd_rename_map_.emplace(AsciiStrToUpper(kv.first), AsciiStrToUpper(kv.second));
    if (!inserted) {
      LOG(ERROR) << "Invalid rename_command flag, trying to give 2 names to a command";
      exit(1);
    }
  }

  for (string name : GetFlag(FLAGS_restricted_commands)) {
    restricted_cmds_.emplace(AsciiStrToUpper(name));
  }
}

void CommandRegistry::Init(unsigned int thread_count) {
  for (auto& [_, cmd] : cmd_map_) {
    cmd.Init(thread_count);
  }
}

CommandRegistry& CommandRegistry::operator<<(CommandId cmd) {
  string k = string(cmd.name());

  absl::InlinedVector<std::string_view, 2> maybe_subcommand = StrSplit(cmd.name(), " ");
  const bool is_sub_command = maybe_subcommand.size() == 2;
  auto it = cmd_rename_map_.find(maybe_subcommand.front());
  if (it != cmd_rename_map_.end()) {
    if (it->second.empty()) {
      return *this;  // Incase of empty string we want to remove the command from registry.
    }
    k = is_sub_command ? absl::StrCat(it->second, " ", maybe_subcommand[1]) : it->second;
  }

  if (restricted_cmds_.find(k) != restricted_cmds_.end()) {
    cmd.SetRestricted(true);
  }

  cmd.SetFamily(family_of_commands_.size() - 1);
  if (!is_sub_command || absl::StartsWith(cmd.name(), "ACL")) {
    cmd.SetBitIndex(1ULL << bit_index_);
    family_of_commands_.back().push_back(std::string(k));
    ++bit_index_;
  } else {
    DCHECK(absl::StartsWith(k, family_of_commands_.back().back()));
    cmd.SetBitIndex(1ULL << (bit_index_ - 1));
  }
  CHECK(cmd_map_.emplace(k, std::move(cmd)).second) << k;

  return *this;
}

void CommandRegistry::StartFamily() {
  family_of_commands_.push_back({});
  bit_index_ = 0;
}

std::string_view CommandRegistry::RenamedOrOriginal(std::string_view orig) const {
  if (cmd_rename_map_.contains(orig)) {
    return cmd_rename_map_.find(orig)->second;
  }
  return orig;
}

CommandRegistry::FamiliesVec CommandRegistry::GetFamilies() {
  return std::move(family_of_commands_);
}

namespace CO {

const char* OptName(CO::CommandOpt fl) {
  using namespace CO;

  switch (fl) {
    case WRITE:
      return "write";
    case READONLY:
      return "readonly";
    case DENYOOM:
      return "denyoom";
    case FAST:
      return "fast";
    case LOADING:
      return "loading";
    case DANGEROUS:
      return "dangerous";
    case ADMIN:
      return "admin";
    case NOSCRIPT:
      return "noscript";
    case BLOCKING:
      return "blocking";
    case HIDDEN:
      return "hidden";
    case INTERLEAVED_KEYS:
      return "interleaved-keys";
    case GLOBAL_TRANS:
      return "global-trans";
    case STORE_LAST_KEY:
      return "store-last-key";
    case VARIADIC_KEYS:
      return "variadic-keys";
    case NO_AUTOJOURNAL:
      return "custom-journal";
    case NO_KEY_TRANSACTIONAL:
      return "no-key-transactional";
    case NO_KEY_TX_SPAN_ALL:
      return "no-key-tx-span-all";
    case IDEMPOTENT:
      return "idempotent";
  }
  return "unknown";
}

}  // namespace CO

}  // namespace dfly
