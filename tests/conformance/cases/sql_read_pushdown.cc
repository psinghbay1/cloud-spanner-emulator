//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <chrono>  // NOLINT
#include <cstdint>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "common/config.h"
#include "tests/conformance/common/database_test_base.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using googlesql_base::testing::StatusIs;

// Two read-write transactions that each SELECT and then write. A SQL read
// share-locks only the primary-key points, prefixes and ranges its WHERE
// pins, so transactions on disjoint keys no longer abort each other.
class SqlReadPushdownTest : public DatabaseTest {
 public:
  void SetUp() override {
    saved_abort_probability_ = config::abort_current_transaction_probability();
    saved_pushdown_ = config::enable_sql_read_pushdown();
    saved_key_range_locking_ = config::enable_key_range_locking();
    // These cases test which keys a read locks, not who wins a conflict: at
    // probability 0 a conflict always aborts the requester.
    config::set_enable_key_range_locking(true);
    config::set_abort_current_transaction_probability(0);
    config::set_enable_sql_read_pushdown(true);
    DatabaseTest::SetUp();
    if (in_prod_env()) {
      GTEST_SKIP() << "Production Spanner waits on lock conflicts.";
    }
    GOOGLESQL_ASSERT_OK(MultiInsert("Accounts", {"id", "status"},
                          {{1, "open"}, {2, "open"}, {3, "closed"},
                           {4, "open"}}));
    GOOGLESQL_ASSERT_OK(MultiInsert("Entries",
                                    {"account_id", "entry_id", "amount"},
                                    {{1, 1, 10}, {1, 2, 20}, {2, 1, 30}}));
    GOOGLESQL_ASSERT_OK(MultiInsert(
        "Members", {"id", "email", "team", "score"},
        {{1, "a@x", "red", 10}, {2, "b@x", "blue", 20},
         {3, "c@x", "red", Null<std::int64_t>()}, {4, "d@x", "blue", 40}}));
    GOOGLESQL_ASSERT_OK(MultiInsert("Tasks", {"id", "task_id", "label"},
                                    {{1, 1, "x"}, {2, 1, "x"}}));
    GOOGLESQL_ASSERT_OK(MultiInsert("Handles", {"id", "handle"},
                                    {{1, "h1"}, {2, "h2"}}));
  }

  void TearDown() override {
    DatabaseTest::TearDown();
    config::set_abort_current_transaction_probability(saved_abort_probability_);
    config::set_enable_sql_read_pushdown(saved_pushdown_);
    config::set_enable_key_range_locking(saved_key_range_locking_);
  }

  absl::Status SetUpDatabase() override {
    return SetSchema({R"(
      CREATE TABLE Accounts(
        id INT64 NOT NULL,
        status STRING(MAX)
      ) PRIMARY KEY (id)
    )",
                      R"(
      CREATE TABLE Entries(
        account_id INT64 NOT NULL,
        entry_id INT64 NOT NULL,
        amount INT64
      ) PRIMARY KEY (account_id, entry_id DESC)
    )",
                      R"(
      CREATE TABLE Members(
        id INT64 NOT NULL,
        email STRING(MAX),
        team STRING(MAX),
        score INT64
      ) PRIMARY KEY (id)
    )",
                      "CREATE INDEX MembersByEmail ON Members(email)",
                      "CREATE NULL_FILTERED INDEX MembersByScore "
                      "ON Members(score DESC)",
                      R"(
      CREATE TABLE Tasks(
        id INT64 NOT NULL,
        task_id INT64 NOT NULL,
        label STRING(MAX)
      ) PRIMARY KEY (id, task_id),
        INTERLEAVE IN PARENT Members ON DELETE CASCADE
    )",
                      "CREATE INDEX TasksByLabel ON Tasks(id, label), "
                      "INTERLEAVE IN Members",
                      R"(
      CREATE TABLE Handles(
        id INT64 NOT NULL,
        handle STRING(MAX)
      ) PRIMARY KEY (id)
    )",
                      "CREATE UNIQUE INDEX HandlesByHandle ON Handles(handle)"});
  }

 protected:
  absl::Status Select(Transaction txn, const std::string& query) {
    return QueryTransaction(std::move(txn), query).status();
  }

  absl::Status Dml(Transaction txn, const std::string& statement) {
    return ExecuteDmlTransaction(std::move(txn), SqlStatement(statement))
        .status();
  }

  absl::Status CommitWithoutMutations(Transaction txn) {
    return CommitTransaction(std::move(txn), {}).status();
  }

  absl::Status DmlWithIds(Transaction txn, const std::string& statement,
                          std::vector<std::int64_t> ids) {
    return ExecuteDmlTransaction(
               std::move(txn),
               SqlStatement(statement,
                            {{"ids", google::cloud::spanner::Value(
                                         std::move(ids))}}))
        .status();
  }

  // A lock conflict can surface at the statement or at commit.
  absl::Status DmlThenCommit(Transaction txn, const std::string& statement) {
    absl::Status status = Dml(txn, statement);
    if (!status.ok()) {
      return status;
    }
    return CommitWithoutMutations(std::move(txn));
  }

 private:
  int saved_abort_probability_ = 0;
  bool saved_pushdown_ = false;
  bool saved_key_range_locking_ = false;
};

TEST_F(SqlReadPushdownTest, ReadThenWriteOnDisjointKeysDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(Select(txn2, "SELECT status FROM Accounts WHERE id = 2"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn2, "UPDATE Accounts SET status = 'b' WHERE id = 2"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));

  EXPECT_THAT(Query("SELECT id, status FROM Accounts WHERE id IN (1, 2) "
                    "ORDER BY id"),
              IsOkAndHoldsRows({{1, "a"}, {2, "b"}}));
}

TEST_F(SqlReadPushdownTest, ReadThenWriteOnSameKeyAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(Select(txn2, "SELECT status FROM Accounts WHERE id = 1"));
  EXPECT_THAT(
      DmlThenCommit(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, NonKeyFilterStillLocksTheWholeTable) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(
      Select(txn2, "SELECT id FROM Accounts WHERE status = 'closed'"));
  EXPECT_THAT(
      DmlThenCommit(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, WriteInsideAReadKeyRangeAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 3"));
  GOOGLESQL_EXPECT_OK(
      Select(txn2, "SELECT status FROM Accounts WHERE id >= 2 AND id <= 3"));
  EXPECT_THAT(
      DmlThenCommit(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 3"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, WriteOutsideAReadKeyRangeDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(
      Select(txn2, "SELECT status FROM Accounts WHERE id BETWEEN 2 AND 3"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, OpenEndedRangeDoesNotLockBelowItsBound) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(Select(txn2, "SELECT status FROM Accounts WHERE id > 3"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, InsertIntoAnOpenEndedReadRangeAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  // The range lock covers rows that do not exist yet.
  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id > 3"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Accounts (id, status) "
                            "VALUES (50, 'open')"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, PrefixAndRangeDoNotLockOtherPrefixes) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT amount FROM Entries "
                             "WHERE account_id = 1 AND entry_id >= 1"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Entries (account_id, entry_id, amount) "
                          "VALUES (2, 5, 5)"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, DescendingColumnRangeCoversInsertInside) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT amount FROM Entries WHERE account_id = 1 "
                             "AND entry_id BETWEEN 3 AND 10"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Entries (account_id, entry_id, "
                            "amount) VALUES (1, 7, 5)"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, DescendingColumnRangeExcludesRowsOutside) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT amount FROM Entries WHERE account_id = 1 "
                             "AND entry_id BETWEEN 3 AND 10"));
  GOOGLESQL_EXPECT_OK(Dml(
      txn2,
      "UPDATE Entries SET amount = 0 WHERE account_id = 1 AND entry_id = 2"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Entries (account_id, entry_id, amount) "
                          "VALUES (1, 11, 5)"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, InListsOnDisjointKeysDoNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(
      Select(txn1, "SELECT status FROM Accounts WHERE id IN (1, 2)"));
  GOOGLESQL_EXPECT_OK(
      Select(txn2, "SELECT status FROM Accounts WHERE id IN UNNEST([3, 4])"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn2, "UPDATE Accounts SET status = 'b' WHERE id = 3"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, InListCoveringTheWrittenKeyAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 2"));
  GOOGLESQL_EXPECT_OK(
      Select(txn2, "SELECT status FROM Accounts WHERE id IN (2, 4)"));
  EXPECT_THAT(
      DmlThenCommit(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 2"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, InsertIntoAReadKeyPrefixAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  // The prefix lock covers rows that do not exist yet.
  GOOGLESQL_EXPECT_OK(
      Select(txn1, "SELECT amount FROM Entries WHERE account_id = 1"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Entries (account_id, entry_id, "
                            "amount) VALUES (1, 99, 5)"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, InsertOutsideAReadKeyPrefixDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(
      Select(txn1, "SELECT amount FROM Entries WHERE account_id = 1"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Entries (account_id, entry_id, amount) "
                          "VALUES (2, 99, 5)"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, FullKeyWithDescendingColumnDoesNotAbortOtherRow) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT amount FROM Entries "
                             "WHERE account_id = 1 AND entry_id = 1"));
  GOOGLESQL_EXPECT_OK(Dml(
      txn2,
      "UPDATE Entries SET amount = 0 WHERE account_id = 1 AND entry_id = 2"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, DmlOnDisjointKeysDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Dml(txn1, "DELETE FROM Accounts WHERE id = 4"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn2, "UPDATE Accounts SET status = 'b' WHERE id IN (2, 3)"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, DmlInsideAnotherDmlKeyRangeAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  absl::Status first =
      Dml(txn1, "UPDATE Accounts SET status = 'a' WHERE id >= 2");
  absl::Status second =
      DmlThenCommit(txn2, "DELETE FROM Accounts WHERE id = 3");
  if (first.ok()) {
    first = CommitWithoutMutations(std::move(txn1));
  }
  // Either transaction may be the one the emulator aborts, but not neither.
  EXPECT_TRUE(first.code() == absl::StatusCode::kAborted ||
              second.code() == absl::StatusCode::kAborted)
      << "first: " << first << ", second: " << second;
}

// A same-table subquery must read every row it asks for, not just the keys
// the DML's WHERE pins.
TEST_F(SqlReadPushdownTest, DmlWithSameTableSubqueryIsCorrect) {
  for (bool pushdown : {true, false}) {
    config::set_enable_sql_read_pushdown(pushdown);
    const int64_t id = pushdown ? 1 : 2;
    GOOGLESQL_EXPECT_OK(DmlThenCommit(
        Transaction(Transaction::ReadWriteOptions()),
        absl::StrCat("UPDATE Accounts SET status = (SELECT status FROM "
                     "Accounts WHERE id = 3) WHERE id = ",
                     id)));
    EXPECT_THAT(
        Query(absl::StrCat("SELECT status FROM Accounts WHERE id = ", id)),
        IsOkAndHoldsRows({{"closed"}}));
  }
  config::set_enable_sql_read_pushdown(true);
  GOOGLESQL_EXPECT_OK(DmlThenCommit(
      Transaction(Transaction::ReadWriteOptions()),
      "DELETE FROM Accounts WHERE id = 4 AND EXISTS (SELECT 1 FROM Accounts "
      "WHERE id = 3)"));
  EXPECT_THAT(Query("SELECT id FROM Accounts WHERE id = 4"),
              IsOkAndHoldsRows({}));
}

TEST_F(SqlReadPushdownTest, DisabledFlagLocksTheWholeTable) {
  config::set_enable_sql_read_pushdown(false);
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1, "SELECT status FROM Accounts WHERE id = 1"));
  GOOGLESQL_EXPECT_OK(Select(txn2, "SELECT status FROM Accounts WHERE id = 2"));
  EXPECT_THAT(
      DmlThenCommit(txn1, "UPDATE Accounts SET status = 'a' WHERE id = 1"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, ResultsMatchWithTheFlagOnAndOff) {
  const std::vector<std::string> queries = {
      "SELECT id, status FROM Accounts WHERE id = 2",
      "SELECT id, status FROM Accounts WHERE id = 99",
      "SELECT id, status FROM Accounts WHERE id IN (1, 3, 99) ORDER BY id",
      "SELECT id, status FROM Accounts WHERE id = 1 OR id = 4 ORDER BY id",
      "SELECT id FROM Accounts WHERE id >= 2 AND id <= 3 ORDER BY id",
      "SELECT id FROM Accounts WHERE status = 'open' AND id = 3",
      "SELECT entry_id, amount FROM Entries WHERE account_id = 1 "
      "ORDER BY entry_id",
      "SELECT amount FROM Entries WHERE account_id = 1 AND entry_id IN (2, 7)",
      "SELECT id FROM Accounts WHERE id > 2 ORDER BY id",
      "SELECT id FROM Accounts WHERE id < 3 ORDER BY id",
      "SELECT id FROM Accounts WHERE id BETWEEN 3 AND 2",
      "SELECT entry_id FROM Entries WHERE account_id = 1 AND entry_id > 1",
      "SELECT entry_id FROM Entries WHERE account_id = 1 "
      "AND entry_id BETWEEN 1 AND 2 ORDER BY entry_id",
      "SELECT a.id, e.amount FROM Accounts a JOIN Entries e "
      "ON a.id = e.account_id WHERE a.id = 1 ORDER BY e.entry_id",
  };
  for (const std::string& query : queries) {
    config::set_enable_sql_read_pushdown(true);
    absl::StatusOr<std::vector<ValueRow>> with_pushdown = Query(query);
    config::set_enable_sql_read_pushdown(false);
    absl::StatusOr<std::vector<ValueRow>> without_pushdown = Query(query);
    GOOGLESQL_ASSERT_OK(with_pushdown) << query;
    GOOGLESQL_ASSERT_OK(without_pushdown) << query;
    EXPECT_EQ(*with_pushdown, *without_pushdown) << query;
  }
}

// Read-only and single-use reads take no locks: they neither abort on a
// read-write transaction's held lock nor cause that transaction to abort.
TEST_F(SqlReadPushdownTest, ReadOnlyReadsIgnoreAHeldWriteLock) {
  Transaction writer = Transaction(Transaction::ReadWriteOptions());
  GOOGLESQL_ASSERT_OK(
      Dml(writer, "UPDATE Accounts SET status = 'held' WHERE id = 1"));

  EXPECT_THAT(Query("SELECT id, status FROM Accounts WHERE id = 1"),
              IsOkAndHoldsRows({{1, "open"}}));
  EXPECT_THAT(Read("Accounts", {"id", "status"}, KeySet::All()),
              IsOkAndHoldsRows({{1, "open"}, {2, "open"}, {3, "closed"},
                                {4, "open"}}));
  Transaction reader = Transaction(Transaction::ReadOnlyOptions());
  EXPECT_THAT(
      QueryTransaction(reader, "SELECT status FROM Accounts WHERE id = 1"),
      IsOkAndHoldsRows({{"open"}}));
  Transaction stale_reader = Transaction(
      Transaction::ReadOnlyOptions(std::chrono::nanoseconds(1)));
  EXPECT_THAT(QueryTransaction(stale_reader, "SELECT status FROM Accounts"),
              googlesql_base::testing::IsOk());

  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(writer));
}

TEST_F(SqlReadPushdownTest, ReadOnlyReadBeforeAWriteDoesNotAbortTheWriter) {
  Transaction reader = Transaction(Transaction::ReadOnlyOptions());
  EXPECT_THAT(
      QueryTransaction(reader, "SELECT status FROM Accounts WHERE id = 1"),
      IsOkAndHoldsRows({{"open"}}));

  Transaction writer = Transaction(Transaction::ReadWriteOptions());
  GOOGLESQL_EXPECT_OK(DmlThenCommit(
      writer, "UPDATE Accounts SET status = 'closed' WHERE id = 1"));

  // The snapshot is unchanged by the later commit.
  EXPECT_THAT(
      QueryTransaction(reader, "SELECT status FROM Accounts WHERE id = 1"),
      IsOkAndHoldsRows({{"open"}}));
}


// Secondary-index reads (stage 3). Every query names its index explicitly.
// "Part 2" = the base-table look-back locks only the rows found in the index
// range; without it the look-back locks the whole base table.

// Part 2: the written base row differs from the one read.
TEST_F(SqlReadPushdownTest, IndexReadThenWriteToAnotherIndexKeyDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email = 'a@x'"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Members (id, email, team, score) "
                          "VALUES (10, 'z@x', 'red', 5)"));
  GOOGLESQL_EXPECT_OK(Dml(
      txn2, "UPDATE Members SET email = 'bb@x' WHERE id = 2"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, IndexReadThenUpdateOfTheMatchedBaseRowAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id, team FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email = 'a@x'"));
  // team is not in the index: only the base-row lock catches this.
  EXPECT_THAT(
      DmlThenCommit(txn2, "UPDATE Members SET team = 'green' WHERE id = 1"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, IndexReadThenInsertOfMatchingValueAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email = 'a@x'"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Members (id, email) "
                            "VALUES (11, 'a@x')"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, EmptyIndexReadThenInsertOfMatchingValueAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  EXPECT_THAT(QueryTransaction(txn1,
                               "SELECT id FROM Members@{FORCE_INDEX="
                               "MembersByEmail} WHERE email = 'new@x'"),
              IsOkAndHoldsRows({}));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Members (id, email) "
                            "VALUES (12, 'new@x')"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, IndexReadThenUpdateMovingIntoTheValueAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email = 'a@x'"));
  EXPECT_THAT(
      DmlThenCommit(txn2, "UPDATE Members SET email = 'a@x' WHERE id = 2"),
      StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, IndexReadThenUpdateMovingOutOfTheValueAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email = 'a@x'"));
  EXPECT_THAT(
      DmlThenCommit(txn2, "UPDATE Members SET email = 'q@x' WHERE id = 1"),
      StatusIs(absl::StatusCode::kAborted));
}

// Part 2: the written base row is outside the index range.
TEST_F(SqlReadPushdownTest, IndexRangeReadThenWriteOutsideDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email BETWEEN 'a' AND 'b'"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Members (id, email) "
                          "VALUES (13, 'm@x')"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn2, "UPDATE Members SET team = 'green' WHERE id = 4"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, IndexRangeReadThenInsertInsideAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE email BETWEEN 'a' AND 'b'"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Members (id, email) "
                            "VALUES (14, 'ab@x')"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, IndexReadWithOnlyResidualFilterLocksBroadly) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  // team is not an index key column: no index range, whole-table fallback.
  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByEmail} WHERE team = 'red'"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Members (id, email, team) "
                            "VALUES (15, 'zz@x', 'blue')"),
              StatusIs(absl::StatusCode::kAborted));
}

// Part 2: the written base row is outside the descending index range.
TEST_F(SqlReadPushdownTest,
       NullFilteredDescendingIndexRangeThenWriteOutsideDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByScore,spanner_emulator."
                             "disable_query_null_filtered_index_check=true} "
                             "WHERE score BETWEEN 15 AND 25"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Members (id, email, score) "
                          "VALUES (16, 'p@x', 50)"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest,
       NullFilteredDescendingIndexRangeThenNullBecomesInsideAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT id FROM Members@{FORCE_INDEX="
                             "MembersByScore,spanner_emulator."
                             "disable_query_null_filtered_index_check=true} "
                             "WHERE score BETWEEN 15 AND 25"));
  // Row 3 has a NULL score, so it enters the NULL_FILTERED index here.
  EXPECT_THAT(
      DmlThenCommit(txn2, "UPDATE Members SET score = 18 WHERE id = 3"),
      StatusIs(absl::StatusCode::kAborted));
}

// Part 2: the written Tasks row is under another parent.
TEST_F(SqlReadPushdownTest, InterleavedIndexReadThenWriteOtherParentDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT task_id FROM Tasks@{FORCE_INDEX="
                             "TasksByLabel} WHERE id = 1 "
                             "AND label = 'x'"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Tasks (id, task_id, label) "
                          "VALUES (2, 2, 'x')"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
}

TEST_F(SqlReadPushdownTest, InterleavedIndexReadThenInsertMatchingAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Select(txn1,
                             "SELECT task_id FROM Tasks@{FORCE_INDEX="
                             "TasksByLabel} WHERE id = 1 "
                             "AND label = 'x'"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Tasks (id, task_id, label) "
                            "VALUES (1, 2, 'x')"),
              StatusIs(absl::StatusCode::kAborted));
}

// Without the hint the same filter is a non-key filter on the base table.
TEST_F(SqlReadPushdownTest, FilterOnIndexedColumnWithoutHintLocksTheWholeTable) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(
      Select(txn1, "SELECT id FROM Members WHERE email = 'a@x'"));
  EXPECT_THAT(DmlThenCommit(txn2,
                            "INSERT INTO Members (id, email) "
                            "VALUES (17, 'z@x')"),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(SqlReadPushdownTest, IndexReadResultsMatchWithTheFlagOnAndOff) {
  const std::vector<std::string> queries = {
      "SELECT id, team FROM Members@{FORCE_INDEX=MembersByEmail} "
      "WHERE email = 'b@x'",
      "SELECT id FROM Members@{FORCE_INDEX=MembersByEmail} "
      "WHERE email = 'none'",
      "SELECT id FROM Members@{FORCE_INDEX=MembersByEmail} "
      "WHERE email IN ('a@x', 'd@x') ORDER BY id",
      "SELECT id FROM Members@{FORCE_INDEX=MembersByEmail} "
      "WHERE email BETWEEN 'a' AND 'c' ORDER BY id",
      "SELECT id FROM Members@{FORCE_INDEX=MembersByEmail} "
      "WHERE email > 'b' AND team = 'blue' ORDER BY id",
      "SELECT id FROM Members@{FORCE_INDEX=MembersByEmail} "
      "WHERE team = 'red' ORDER BY id",
      "SELECT id, score FROM Members@{FORCE_INDEX=MembersByScore,"
      "spanner_emulator.disable_query_null_filtered_index_check=true} "
      "WHERE score >= 20 ORDER BY id",
      "SELECT id FROM Members@{FORCE_INDEX=MembersByScore,"
      "spanner_emulator.disable_query_null_filtered_index_check=true} "
      "WHERE score BETWEEN 5 AND 25 ORDER BY id",
      "SELECT task_id FROM Tasks@{FORCE_INDEX=TasksByLabel} "
      "WHERE id = 1 AND label = 'x'",
      "SELECT id FROM Tasks@{FORCE_INDEX=TasksByLabel} "
      "WHERE label = 'x' ORDER BY id",
  };
  for (const std::string& query : queries) {
    config::set_enable_sql_read_pushdown(true);
    absl::StatusOr<std::vector<ValueRow>> with_pushdown = Query(query);
    config::set_enable_sql_read_pushdown(false);
    absl::StatusOr<std::vector<ValueRow>> without_pushdown = Query(query);
    GOOGLESQL_ASSERT_OK(with_pushdown) << query;
    GOOGLESQL_ASSERT_OK(without_pushdown) << query;
    EXPECT_EQ(*with_pushdown, *without_pushdown) << query;
  }
}

TEST_F(SqlReadPushdownTest, InUnnestDmlOnDisjointKeysDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(
      Dml(txn1, "DELETE FROM Accounts WHERE id IN UNNEST([4])"));
  GOOGLESQL_EXPECT_OK(DmlWithIds(
      txn2, "UPDATE Accounts SET status = 'b' WHERE id IN UNNEST(@ids)",
      {2, 3}));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, InUnnestDmlOnOverlappingKeysAborts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(DmlWithIds(
      txn1, "UPDATE Accounts SET status = 'a' WHERE id IN UNNEST(@ids)",
      {2, 3}));
  absl::Status second = DmlThenCommit(
      txn2, "DELETE FROM Accounts WHERE id IN UNNEST([3, 4])");
  absl::Status first = CommitWithoutMutations(txn1);
  EXPECT_TRUE(absl::IsAborted(first) || absl::IsAborted(second))
      << first << " / " << second;
}

TEST_F(SqlReadPushdownTest, InsertOnConflictOnDisjointKeysDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Dml(txn1,
                          "INSERT INTO Accounts(id, status) VALUES (1, 'u'), "
                          "(10, 'n') ON CONFLICT(id) DO UPDATE "
                          "SET status = excluded.status"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Accounts(id, status) VALUES (2, 'u'), "
                          "(11, 'n') ON CONFLICT(id) DO NOTHING"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, InsertOnConflictOnTheSameKeyConflicts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Dml(txn1,
                          "INSERT INTO Accounts(id, status) VALUES (12, 'a') "
                          "ON CONFLICT(id) DO UPDATE SET status = "
                          "excluded.status"));
  absl::Status second =
      DmlThenCommit(txn2,
                    "INSERT INTO Accounts(id, status) VALUES (12, 'b') "
                    "ON CONFLICT(id) DO UPDATE SET status = excluded.status");
  absl::Status first = CommitWithoutMutations(txn1);
  EXPECT_FALSE(first.ok() && second.ok())
      << first << " / " << second;
}

TEST_F(SqlReadPushdownTest,
       InsertOnConflictOnUniqueIndexWithDisjointValuesDoesNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Dml(txn1,
                          "INSERT INTO Handles(id, handle) VALUES (3, 'h3') "
                          "ON CONFLICT(handle) DO NOTHING"));
  GOOGLESQL_EXPECT_OK(Dml(txn2,
                          "INSERT INTO Handles(id, handle) VALUES (4, 'h4') "
                          "ON CONFLICT(handle) DO NOTHING"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest,
       InsertOnConflictOnUniqueIndexWithTheSameValueConflicts) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(Dml(txn1,
                          "INSERT INTO Handles(id, handle) VALUES (5, 'h5') "
                          "ON CONFLICT(handle) DO NOTHING"));
  absl::Status second =
      DmlThenCommit(txn2,
                    "INSERT INTO Handles(id, handle) VALUES (6, 'h5') "
                    "ON CONFLICT(handle) DO NOTHING");
  absl::Status first = CommitWithoutMutations(txn1);
  EXPECT_FALSE(first.ok() && second.ok()) << first << " / " << second;
}

// A plain INSERT pins its duplicate-key scan to the keys it writes. Every key
// shape must still write all its rows: one row, several rows, key columns
// listed last and out of order, and a parameter.
TEST_F(SqlReadPushdownTest, PlainInsertWritesEveryRow) {
  GOOGLESQL_EXPECT_OK(DmlThenCommit(
      Transaction(Transaction::ReadWriteOptions()),
      "INSERT INTO Accounts(id, status) VALUES (40, 'a')"));
  GOOGLESQL_EXPECT_OK(DmlThenCommit(
      Transaction(Transaction::ReadWriteOptions()),
      "INSERT INTO Entries(amount, entry_id, account_id) "
      "VALUES (7, 5, 40), (8, 6, 40)"));
  Transaction with_parameter = Transaction(Transaction::ReadWriteOptions());
  GOOGLESQL_EXPECT_OK(
      ExecuteDmlTransaction(
          with_parameter,
          SqlStatement("INSERT INTO Accounts(id, status) VALUES (@id, 'b')",
                       {{"id", google::cloud::spanner::Value(
                                   std::int64_t{41})}}))
          .status());
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(std::move(with_parameter)));
  EXPECT_THAT(Query("SELECT id, status FROM Accounts WHERE id >= 40 "
                    "ORDER BY id"),
              IsOkAndHoldsRows({{40, "a"}, {41, "b"}}));
  EXPECT_THAT(Query("SELECT entry_id, amount FROM Entries "
                    "WHERE account_id = 40 ORDER BY entry_id"),
              IsOkAndHoldsRows({{5, 7}, {6, 8}}));
}

// Narrowing must not hide the existing row a duplicate key collides with.
TEST_F(SqlReadPushdownTest, PlainInsertOfAnExistingKeyFails) {
  for (bool pushdown : {true, false}) {
    config::set_enable_sql_read_pushdown(pushdown);
    EXPECT_THAT(DmlThenCommit(Transaction(Transaction::ReadWriteOptions()),
                              "INSERT INTO Accounts(id, status) "
                              "VALUES (99, 'n'), (1, 'dup')"),
                StatusIs(absl::StatusCode::kAlreadyExists))
        << "pushdown=" << pushdown;
    EXPECT_THAT(DmlThenCommit(Transaction(Transaction::ReadWriteOptions()),
                              "INSERT INTO Entries(amount, entry_id, "
                              "account_id) VALUES (0, 2, 1)"),
                StatusIs(absl::StatusCode::kAlreadyExists))
        << "pushdown=" << pushdown;
  }
  EXPECT_THAT(Query("SELECT status FROM Accounts WHERE id IN (1, 99)"),
              IsOkAndHoldsRows({{"open"}}));
}

// Two plain INSERTs of different new keys lock only those keys.
TEST_F(SqlReadPushdownTest, PlainInsertsOfDisjointKeysDoNotAbort) {
  Transaction txn1 = Transaction(Transaction::ReadWriteOptions());
  Transaction txn2 = Transaction(Transaction::ReadWriteOptions());

  GOOGLESQL_EXPECT_OK(
      Dml(txn1, "INSERT INTO Accounts(id, status) VALUES (50, 'a')"));
  GOOGLESQL_EXPECT_OK(
      Dml(txn2, "INSERT INTO Accounts(status, id) VALUES ('b', 51)"));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn1));
  GOOGLESQL_EXPECT_OK(CommitWithoutMutations(txn2));
}

TEST_F(SqlReadPushdownTest, DmlResultsMatchWithTheFlagOnAndOff) {
  const std::vector<std::string> statements = {
      "UPDATE Accounts SET status = 'x' WHERE id IN UNNEST([2, 3, 99]) "
      "THEN RETURN id, status",
      "DELETE FROM Accounts WHERE id IN UNNEST(ARRAY<INT64>[]) "
      "THEN RETURN id",
      "DELETE FROM Accounts WHERE id IN UNNEST([1, NULL]) THEN RETURN id",
      "UPDATE Entries SET amount = 0 WHERE account_id IN UNNEST([1]) "
      "AND entry_id IN UNNEST([2, 5]) THEN RETURN account_id, entry_id",
      "INSERT INTO Accounts(id, status) VALUES (1, 'u'), (20, 'n') "
      "ON CONFLICT(id) DO UPDATE SET status = excluded.status "
      "THEN RETURN WITH ACTION AS action id, status",
      "INSERT INTO Accounts(id, status) VALUES (2, 'u'), (21, 'n') "
      "ON CONFLICT(id) DO NOTHING THEN RETURN id, status",
      "INSERT INTO Entries(account_id, entry_id, amount) VALUES (1, 1, 5), "
      "(2, 2, 6) ON CONFLICT(account_id, entry_id) DO UPDATE "
      "SET amount = excluded.amount THEN RETURN account_id, entry_id, amount",
      "INSERT INTO Handles(id, handle) VALUES (7, 'h1'), (8, 'h8') "
      "ON CONFLICT(handle) DO NOTHING THEN RETURN id, handle",
      "INSERT INTO Accounts(id, status) VALUES (30, 'n') THEN RETURN id",
      "INSERT INTO Entries(amount, entry_id, account_id) VALUES (1, 9, 2), "
      "(2, 8, 3) THEN RETURN account_id, entry_id, amount",
  };
  for (const std::string& statement : statements) {
    std::vector<absl::StatusOr<std::vector<ValueRow>>> results;
    for (bool pushdown : {true, false}) {
      config::set_enable_sql_read_pushdown(pushdown);
      Transaction txn = Transaction(Transaction::ReadWriteOptions());
      results.push_back(QueryTransaction(txn, statement));
      GOOGLESQL_EXPECT_OK(Rollback(txn)) << statement;
    }
    GOOGLESQL_ASSERT_OK(results[0]) << statement;
    GOOGLESQL_ASSERT_OK(results[1]) << statement;
    EXPECT_THAT(*results[0], testing::UnorderedElementsAreArray(*results[1]))
        << statement;
  }
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
