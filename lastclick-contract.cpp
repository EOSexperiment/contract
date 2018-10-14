#include <eosiolib/eosio.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/transaction.hpp>
#include <eosiolib/time.hpp>

using namespace eosio;
const auto INIT_GAME_DELAY = 100 * 60; // 100m
const auto STEP_DELAY = 60;            // 1m
const auto END_GAME_DELAY = 60;        //1 m
class lastclick : public eosio::contract
{
public:
  using contract::contract;
  lastclick(account_name self) : contract(self), winners_table(self, self), global_table(self, self), game_table(self, self)
  {
  }
  struct [[eosio::table]] global
  {
    uint64_t games;

    uint64_t primary_key() const { return 0; }
    EOSLIB_SERIALIZE(global, (games))
  };
  typedef eosio::multi_index<N(global), global> global_index;
  global_index global_table;
  struct [[eosio::table]] player
  {
    account_name name;

    uint64_t primary_key() const { return name; }
    EOSLIB_SERIALIZE(player, (name))
  };
  typedef eosio::multi_index<N(players), player> players_index;

  struct [[eosio::table]] game
  {
    uint64_t players;
    uint64_t bets;
    asset value;
    account_name winner;
    eosio::time_point_sec endtime;

    uint64_t primary_key() const { return 0; }
    EOSLIB_SERIALIZE(game, (players)(bets)(value)(winner)(endtime))
  };
  typedef eosio::multi_index<N(games), game> game_index;
  game_index game_table;

  struct [[eosio::table]] winners
  {
    eosio::time_point_sec time;
    account_name name;
    asset value;

    auto primary_key() const { return time.utc_seconds; }
    uint64_t by_time_desc() const { return 0xffffffffffffffff - time.utc_seconds; }
    EOSLIB_SERIALIZE(winners, (time)(name)(value))
  };

  typedef eosio::multi_index<N(winners), winners,
                             indexed_by<N(time), const_mem_fun<winners, uint64_t, &winners::by_time_desc>>>
      winners_index;
  winners_index winners_table;
  [[eosio::action]] void claim() {
    auto itr = game_table.begin();
    eosio_assert(itr != game_table.end(), "game should start");
    eosio_assert(eosio::time_point_sec(now()) > (itr->endtime), "not yet");
    action(
        permission_level(_self, N(active)),
        N(eosio.token), N(transfer),
        std::make_tuple(_self, itr->winner, itr->value, std::string("")))
        .send();
    winners_table.emplace(_self, [&](auto &t) {
      t.time = eosio::time_point_sec(now());
      t.name = itr->winner;
      t.value = itr->value;
    });

    game_table.erase(itr);
  };
  [[eosio::action]] void clearplayers(uint64_t gameid) {
    require_auth(_self);
    eosio_assert(gameid < global_table.begin()->games, "cant clear players of active game");
    players_index players_table(_self, gameid);

    for (auto itr = players_table.begin(); itr != players_table.end();)
    {
      itr = players_table.erase(itr);
    }
  };
  void enqueueClaimAction(uint32_t delay)
  {
    cancel_deferred(_self);
    auto trx = transaction();
    trx.actions.emplace_back(
        action(
            permission_level(_self, N(active)),
            _self, N(claim),
            std::make_tuple()));
    trx.delay_sec = delay + 2;
    trx.send(_self, _self);
  }
  void proceedComission(std::string memo, uint64_t bet, account_name id)
  {
    account_name referer = string_to_name(memo.c_str());
    if (is_account(referer))
    {
      uint64_t referals = getReferalAmount(bet);
      auto trx = transaction();
      trx.actions.emplace_back(
          action(
          permission_level(_self, N(active)),
          N(eosio.token), N(transfer),
          std::make_tuple(_self, referer, asset(referals, S(4, EOS)), std::string("Referral payment from eosEXPERIMENT.IO ==============> the most fair  game on EOS"))));
      trx.send(id, _self);
    }
  }
  uint32_t getDelayForPot(uint64_t players)
  {
    if (players >= INIT_GAME_DELAY / STEP_DELAY)
    {
      return END_GAME_DELAY;
    }
    return INIT_GAME_DELAY - STEP_DELAY * (players - 1);
  }
  uint64_t getAmountByBets(uint64_t bets)
  {
    return ((bets / 100) + 1) * 10000;
  }
  uint64_t getAmountWithCommision(uint64_t amount)
  {
    return amount * 105 / 100;
  }
  uint64_t getReferalAmount(uint64_t amount)
  {
    return amount / 100;
  }
  // eosio.token transfer handler
  void transfer(const account_name from, const account_name to, asset quantity, std::string memo)
  {
    if (from == _self || to != _self)
    {
      return;
    }
    eosio_assert(quantity.symbol == S(4, EOS), "only accepts EOS for deposits");
    eosio_assert(quantity.is_valid(), "invalid asset");
    eosio_assert(quantity.amount > 0, "invalid asset");

    if (global_table.begin() == global_table.end())
    {
      global_table.emplace(_self, [&](global &g) {
        g.games = 0;
      });
    }
    uint64_t gameId = global_table.begin()->games;
    uint32_t delay;
    uint64_t bet;
    if (game_table.begin() == game_table.end())
    {
      bet = getAmountByBets(0);
      eosio_assert(quantity.amount == getAmountWithCommision(bet), "wrong bet");
      global_table.modify(global_table.begin(), 0, [&](global &g) {
        g.games++;
      });
      gameId = global_table.begin()->games;
      players_index players_table(_self, gameId);
      players_table.emplace(_self, [&](player &g) {
        g.name = from;
      });
      game_table.emplace(_self, [&](game &g) {
        g.players = 1;
        g.bets = 1;
        g.value = asset(bet, S(4, EOS));
        g.winner = from;

        delay = getDelayForPot(g.players);
        g.endtime = eosio::time_point_sec(now() + delay);
      });
    }
    else
    {
      bet = getAmountByBets(game_table.begin()->bets);
      eosio_assert(quantity.amount == getAmountWithCommision(bet), "wrong bet");

      players_index players_table(_self, gameId);
      eosio_assert(eosio::time_point_sec(now()) < game_table.begin()->endtime, "game finished");
      bool newPlayer = false;
      if (game_table.begin()->players < 100)
      {
        if (players_table.find(from) == players_table.end())
        {
          players_table.emplace(_self, [&](player &g) {
            g.name = from;
          });
          newPlayer = true;
        }
      }

      game_table.modify(game_table.begin(), 0, [&](game &g) {
        g.value += asset(bet, S(4, EOS));
        g.winner = from;
        g.bets++;
        if (newPlayer)
        {
          g.players++;
        }
        delay = getDelayForPot(g.players);
        g.endtime = eosio::time_point_sec(now() + delay);
      });
    }
    enqueueClaimAction(delay);
    proceedComission(memo, bet, from);
  }
};

#undef EOSIO_ABI

#define EOSIO_ABI(TYPE, MEMBERS)                                                                                         \
  extern "C"                                                                                                             \
  {                                                                                                                      \
    void apply(uint64_t receiver, uint64_t code, uint64_t action)                                                        \
    {                                                                                                                    \
      if (action == N(onerror))                                                                                          \
      {                                                                                                                  \
        /* onerror is only valid if it is for the "eosio" code account and authorized by "eosio"'s "active permission */ \
        eosio_assert(code == N(eosio), "onerror action's are only valid from the \"eosio\" system account");             \
      }                                                                                                                  \
      if (action == N(transfer))                                                                                         \
      {                                                                                                                  \
        /* onerror is only valid if it is for the "eosio" code account and authorized by "eosio"'s "active permission */ \
        eosio_assert(code == N(eosio.token), "transfer action's are only valid from the \"eosio.token\" account");       \
      }                                                                                                                  \
      auto self = receiver;                                                                                              \
      if (code == self || code == N(eosio.token) || action == N(onerror))                                                \
      {                                                                                                                  \
        TYPE thiscontract(self);                                                                                         \
        switch (action)                                                                                                  \
        {                                                                                                                \
          EOSIO_API(TYPE, MEMBERS)                                                                                       \
        }                                                                                                                \
        /* does not allow destructor of thiscontract to run: eosio_exit(0); */                                           \
      }                                                                                                                  \
    }                                                                                                                    \
  }
EOSIO_ABI(lastclick, (transfer)(claim)(clearplayers))
