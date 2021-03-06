//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/TopDialogManager.h"

#include "td/telegram/ConfigShared.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/Global.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/misc.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/net/NetQueryDispatcher.h"
#include "td/telegram/StateManager.h"
#include "td/telegram/Td.h"

#include "td/utils/misc.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/tl_helpers.h"

#include "td/telegram/telegram_api.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace td {

static CSlice top_dialog_category_name(TopDialogCategory category) {
  switch (category) {
    case TopDialogCategory::Correspondent:
      return CSlice("correspondent");
    case TopDialogCategory::BotPM:
      return CSlice("bot_pm");
    case TopDialogCategory::BotInline:
      return CSlice("bot_inline");
    case TopDialogCategory::Group:
      return CSlice("group");
    case TopDialogCategory::Channel:
      return CSlice("channel");
    case TopDialogCategory::Call:
      return CSlice("call");
    default:
      UNREACHABLE();
  }
}

static TopDialogCategory top_dialog_category_from_telegram_api(const telegram_api::TopPeerCategory &category) {
  switch (category.get_id()) {
    case telegram_api::topPeerCategoryCorrespondents::ID:
      return TopDialogCategory::Correspondent;
    case telegram_api::topPeerCategoryBotsPM::ID:
      return TopDialogCategory::BotPM;
    case telegram_api::topPeerCategoryBotsInline::ID:
      return TopDialogCategory::BotInline;
    case telegram_api::topPeerCategoryGroups::ID:
      return TopDialogCategory::Group;
    case telegram_api::topPeerCategoryChannels::ID:
      return TopDialogCategory::Channel;
    case telegram_api::topPeerCategoryPhoneCalls::ID:
      return TopDialogCategory::Call;
    default:
      UNREACHABLE();
  }
}

static tl_object_ptr<telegram_api::TopPeerCategory> top_dialog_category_as_telegram_api(TopDialogCategory category) {
  switch (category) {
    case TopDialogCategory::Correspondent:
      return make_tl_object<telegram_api::topPeerCategoryCorrespondents>();
    case TopDialogCategory::BotPM:
      return make_tl_object<telegram_api::topPeerCategoryBotsPM>();
    case TopDialogCategory::BotInline:
      return make_tl_object<telegram_api::topPeerCategoryBotsInline>();
    case TopDialogCategory::Group:
      return make_tl_object<telegram_api::topPeerCategoryGroups>();
    case TopDialogCategory::Channel:
      return make_tl_object<telegram_api::topPeerCategoryChannels>();
    case TopDialogCategory::Call:
      return make_tl_object<telegram_api::topPeerCategoryPhoneCalls>();
    default:
      UNREACHABLE();
  }
}

void TopDialogManager::on_dialog_used(TopDialogCategory category, DialogId dialog_id, int32 date) {
  if (!is_active_) {
    return;
  }
  auto pos = static_cast<size_t>(category);
  CHECK(pos < by_category_.size());
  auto &top_dialogs = by_category_[pos];

  top_dialogs.is_dirty = true;
  auto it = std::find_if(top_dialogs.dialogs.begin(), top_dialogs.dialogs.end(),
                         [&](auto &top_dialog) { return top_dialog.dialog_id == dialog_id; });
  if (it == top_dialogs.dialogs.end()) {
    TopDialog top_dialog;
    top_dialog.dialog_id = dialog_id;
    top_dialogs.dialogs.push_back(top_dialog);
    it = top_dialogs.dialogs.end() - 1;
  }

  auto delta = rating_add(date, top_dialogs.rating_timestamp);
  it->rating += delta;
  while (it != top_dialogs.dialogs.begin()) {
    auto next = std::prev(it);
    if (*next < *it) {
      break;
    }
    std::swap(*next, *it);
    it = next;
  }

  LOG(INFO) << "Update " << top_dialog_category_name(category) << " rating of " << dialog_id << " by " << delta;

  if (!first_unsync_change_) {
    first_unsync_change_ = Timestamp::now_cached();
  }
  loop();
}

void TopDialogManager::remove_dialog(TopDialogCategory category, DialogId dialog_id,
                                     tl_object_ptr<telegram_api::InputPeer> input_peer) {
  if (!is_active_) {
    return;
  }

  auto pos = static_cast<size_t>(category);
  CHECK(pos < by_category_.size());
  auto &top_dialogs = by_category_[pos];

  LOG(INFO) << "Remove " << top_dialog_category_name(category) << " rating of " << dialog_id;

  if (input_peer != nullptr) {
    auto query =
        telegram_api::contacts_resetTopPeerRating(top_dialog_category_as_telegram_api(category), std::move(input_peer));
    auto net_query = G()->net_query_creator().create(create_storer(query));
    G()->net_query_dispatcher().dispatch_with_callback(std::move(net_query), actor_shared(this, 1));
  }

  auto it = std::find_if(top_dialogs.dialogs.begin(), top_dialogs.dialogs.end(),
                         [&](auto &top_dialog) { return top_dialog.dialog_id == dialog_id; });
  if (it == top_dialogs.dialogs.end()) {
    return;
  }

  top_dialogs.is_dirty = true;
  top_dialogs.dialogs.erase(it);
  if (!first_unsync_change_) {
    first_unsync_change_ = Timestamp::now_cached();
  }
  loop();
}

void TopDialogManager::get_top_dialogs(TopDialogCategory category, size_t limit, Promise<vector<DialogId>> promise) {
  if (!is_active_) {
    promise.set_error(Status::Error(400, "Not supported without chat info database"));
    return;
  }
  GetTopDialogsQuery query;
  query.category = category;
  query.limit = limit;
  query.promise = std::move(promise);
  pending_get_top_dialogs_.push_back(std::move(query));
  loop();
}

void TopDialogManager::update_rating_e_decay() {
  if (!is_active_) {
    return;
  }
  rating_e_decay_ = G()->shared_config().get_option_integer("rating_e_decay", rating_e_decay_);
}

template <class T>
void parse(TopDialogManager::TopDialog &top_dialog, T &parser) {
  using ::td::parse;
  parse(top_dialog.dialog_id, parser);
  parse(top_dialog.rating, parser);
}

template <class T>
void store(const TopDialogManager::TopDialog &top_dialog, T &storer) {
  using ::td::store;
  store(top_dialog.dialog_id, storer);
  store(top_dialog.rating, storer);
}

template <class T>
void parse(TopDialogManager::TopDialogs &top_dialogs, T &parser) {
  using ::td::parse;
  parse(top_dialogs.rating_timestamp, parser);
  parse(top_dialogs.dialogs, parser);
}

template <class T>
void store(const TopDialogManager::TopDialogs &top_dialogs, T &storer) {
  using ::td::store;
  store(top_dialogs.rating_timestamp, storer);
  store(top_dialogs.dialogs, storer);
}

double TopDialogManager::rating_add(double now, double rating_timestamp) const {
  return std::exp((now - rating_timestamp) / rating_e_decay_);
}

double TopDialogManager::current_rating_add(double rating_timestamp) const {
  return rating_add(G()->server_time_cached(), rating_timestamp);
}

void TopDialogManager::normalize_rating() {
  for (auto &top_dialogs : by_category_) {
    auto div_by = current_rating_add(top_dialogs.rating_timestamp);
    top_dialogs.rating_timestamp = G()->server_time_cached();
    for (auto &dialog : top_dialogs.dialogs) {
      dialog.rating /= div_by;
    }
    top_dialogs.is_dirty = true;
  }
  db_sync_state_ = SyncState::None;
}

void TopDialogManager::do_get_top_dialogs(GetTopDialogsQuery &&query) {
  auto pos = static_cast<size_t>(query.category);
  CHECK(pos < by_category_.size());
  auto &top_dialogs = by_category_[pos];

  auto limit = std::min({query.limit, MAX_TOP_DIALOGS_LIMIT, top_dialogs.dialogs.size()});

  vector<DialogId> dialog_ids = transform(top_dialogs.dialogs, [](const auto &x) { return x.dialog_id; });

  auto promise = PromiseCreator::lambda([query = std::move(query), dialog_ids, limit](Result<Unit>) mutable {
    vector<DialogId> result;
    result.reserve(limit);
    for (auto dialog_id : dialog_ids) {
      if (dialog_id.get_type() == DialogType::User) {
        auto user_id = dialog_id.get_user_id();
        if (G()->td().get_actor_unsafe()->contacts_manager_->is_user_deleted(user_id)) {
          LOG(INFO) << "Skip deleted " << user_id;
          continue;
        }
        if (G()->td().get_actor_unsafe()->contacts_manager_->get_my_id("do_get_top_dialogs") == user_id) {
          LOG(INFO) << "Skip self " << user_id;
          continue;
        }
      }

      result.push_back(dialog_id);
      if (result.size() == limit) {
        break;
      }
    }

    query.promise.set_value(std::move(result));
  });
  send_closure(G()->messages_manager(), &MessagesManager::load_dialogs, std::move(dialog_ids), std::move(promise));
}

void TopDialogManager::do_get_top_peers() {
  LOG(INFO) << "Send get top peers request";
  using telegram_api::contacts_getTopPeers;

  std::vector<uint32> ids;
  for (auto &category : by_category_) {
    for (auto &top_dialog : category.dialogs) {
      auto dialog_id = top_dialog.dialog_id;
      switch (dialog_id.get_type()) {
        case DialogType::Channel:
          ids.push_back(dialog_id.get_channel_id().get());
          break;
        case DialogType::User:
          ids.push_back(dialog_id.get_user_id().get());
          break;
        case DialogType::Chat:
          ids.push_back(dialog_id.get_chat_id().get());
          break;
        default:
          break;
      }
    }
  }

  int32 hash = get_vector_hash(ids);

  int32 flags = contacts_getTopPeers::CORRESPONDENTS_MASK | contacts_getTopPeers::BOTS_PM_MASK |
                contacts_getTopPeers::BOTS_INLINE_MASK | contacts_getTopPeers::GROUPS_MASK |
                contacts_getTopPeers::CHANNELS_MASK | contacts_getTopPeers::PHONE_CALLS_MASK;

  contacts_getTopPeers query{
      flags,           true /*correspondents*/, true /*bot_pm*/, true /*bot_inline */, true /*phone_calls*/,
      true /*groups*/, true /*channels*/,       0 /*offset*/,    100 /*limit*/,        hash};
  auto net_query = G()->net_query_creator().create(create_storer(query));
  G()->net_query_dispatcher().dispatch_with_callback(std::move(net_query), actor_shared(this));
}

void TopDialogManager::on_result(NetQueryPtr net_query) {
  if (get_link_token() == 1) {
    return;
  }
  SCOPE_EXIT {
    loop();
  };
  normalize_rating();  // once a day too
  last_server_sync_ = Timestamp::now();
  server_sync_state_ = SyncState::Ok;
  G()->td_db()->get_binlog_pmc()->set("top_dialogs_ts", to_string(static_cast<uint32>(Clocks::system())));

  auto r_top_peers = fetch_result<telegram_api::contacts_getTopPeers>(std::move(net_query));
  if (r_top_peers.is_error()) {
    LOG(ERROR) << "contacts_getTopPeers failed: " << r_top_peers.error();
    return;
  }
  auto top_peers_parent = r_top_peers.move_as_ok();
  LOG(INFO) << "contacts_getTopPeers returned " << to_string(top_peers_parent);
  if (top_peers_parent->get_id() == telegram_api::contacts_topPeersNotModified::ID) {
    return;
  }

  CHECK(top_peers_parent->get_id() == telegram_api::contacts_topPeers::ID);
  auto top_peers = move_tl_object_as<telegram_api::contacts_topPeers>(std::move(top_peers_parent));

  send_closure(G()->contacts_manager(), &ContactsManager::on_get_users, std::move(top_peers->users_));
  send_closure(G()->contacts_manager(), &ContactsManager::on_get_chats, std::move(top_peers->chats_));
  for (auto &category : top_peers->categories_) {
    auto dialog_category = top_dialog_category_from_telegram_api(*category->category_);
    auto pos = static_cast<size_t>(dialog_category);
    CHECK(pos < by_category_.size());
    auto &top_dialogs = by_category_[pos];

    top_dialogs.is_dirty = true;
    top_dialogs.dialogs.clear();
    for (auto &top_peer : category->peers_) {
      TopDialog top_dialog;
      top_dialog.dialog_id = DialogId(top_peer->peer_);
      top_dialog.rating = top_peer->rating_;
      top_dialogs.dialogs.push_back(std::move(top_dialog));
    }
  }
  db_sync_state_ = SyncState::None;
}

void TopDialogManager::do_save_top_dialogs() {
  LOG(INFO) << "Save top chats";
  for (size_t top_dialog_category_i = 0; top_dialog_category_i < by_category_.size(); top_dialog_category_i++) {
    auto top_dialog_category = TopDialogCategory(top_dialog_category_i);
    auto key = PSTRING() << "top_dialogs#" << top_dialog_category_name(top_dialog_category);

    auto &top_dialogs = by_category_[top_dialog_category_i];
    if (!top_dialogs.is_dirty) {
      continue;
    }
    top_dialogs.is_dirty = false;

    G()->td_db()->get_binlog_pmc()->set(key, log_event_store(top_dialogs).as_slice().str());
  }
  db_sync_state_ = SyncState::Ok;
  first_unsync_change_ = Timestamp();
}

void TopDialogManager::start_up() {
  if (!G()->parameters().use_chat_info_db) {
    G()->td_db()->get_binlog_pmc()->erase_by_prefix("top_dialogs");
    is_active_ = false;
    return;
  }
  is_active_ = true;

  auto di_top_dialogs_ts = G()->td_db()->get_binlog_pmc()->get("top_dialogs_ts");
  if (!di_top_dialogs_ts.empty()) {
    last_server_sync_ = Timestamp::in(to_integer<uint32>(di_top_dialogs_ts) - Clocks::system());
    if (last_server_sync_.is_in_past()) {
      server_sync_state_ = SyncState::Ok;
    }
  }
  update_rating_e_decay();

  for (size_t top_dialog_category_i = 0; top_dialog_category_i < by_category_.size(); top_dialog_category_i++) {
    auto top_dialog_category = TopDialogCategory(top_dialog_category_i);
    auto key = PSTRING() << "top_dialogs#" << top_dialog_category_name(top_dialog_category);
    auto value = G()->td_db()->get_binlog_pmc()->get(key);

    auto &top_dialogs = by_category_[top_dialog_category_i];
    top_dialogs.is_dirty = false;
    if (value.empty()) {
      continue;
    }
    log_event_parse(top_dialogs, value).ensure();
  }
  normalize_rating();
  db_sync_state_ = SyncState::Ok;

  send_closure(G()->state_manager(), &StateManager::wait_first_sync,
               PromiseCreator::event(self_closure(this, &TopDialogManager::on_first_sync)));

  loop();
}

void TopDialogManager::on_first_sync() {
  was_first_sync_ = true;
  loop();
}

void TopDialogManager::loop() {
  if (!is_active_) {
    return;
  }

  if (!pending_get_top_dialogs_.empty()) {
    for (auto &query : pending_get_top_dialogs_) {
      do_get_top_dialogs(std::move(query));
    }
    pending_get_top_dialogs_.clear();
  }

  // server sync
  Timestamp server_sync_timeout;
  if (server_sync_state_ == SyncState::Ok) {
    server_sync_timeout = Timestamp::at(last_server_sync_.at() + SERVER_SYNC_DELAY);
    if (server_sync_timeout.is_in_past()) {
      server_sync_state_ = SyncState::None;
    }
  }

  Timestamp wakeup_timeout;
  if (server_sync_state_ == SyncState::Ok) {
    wakeup_timeout.relax(server_sync_timeout);
  } else if (server_sync_state_ == SyncState::None && was_first_sync_) {
    server_sync_state_ = SyncState::Pending;
    do_get_top_peers();
  }

  // db sync
  Timestamp db_sync_timeout;
  if (db_sync_state_ == SyncState::Ok) {
    if (first_unsync_change_) {
      db_sync_timeout = Timestamp::at(first_unsync_change_.at() + DB_SYNC_DELAY);
      if (db_sync_timeout.is_in_past()) {
        db_sync_state_ = SyncState::None;
      }
    }
  }

  if (db_sync_state_ == SyncState::Ok) {
    wakeup_timeout.relax(db_sync_timeout);
  } else if (db_sync_state_ == SyncState::None) {
    if (server_sync_state_ == SyncState::Ok) {
      do_save_top_dialogs();
    }
  }

  if (wakeup_timeout) {
    LOG(INFO) << "Wakeup in: " << wakeup_timeout.in();
    set_timeout_at(wakeup_timeout.at());
  } else {
    LOG(INFO) << "Wakeup: never";
    cancel_timeout();
  }
}

}  // namespace td
