#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/storage/object_cache.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp" 

using namespace duckdb;
using namespace std;

TEST_CASE("Test Hash Join Caching", "[api][hashjoin]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &client_context = *con.context;
	
	// Create tables with VARCHAR keys to avoid PerfectHashJoin optimization
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE BuildTable (id VARCHAR PRIMARY KEY, val INT);"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE Probes (id VARCHAR);"));
	
	// Populate BuildTable (Build side)
	REQUIRE_NO_FAIL(con.Query("INSERT INTO BuildTable SELECT range::VARCHAR, range * 10 FROM range(1000);"));
	
	// Populate Probes (Probe side)
	REQUIRE_NO_FAIL(con.Query("INSERT INTO Probes SELECT range::VARCHAR FROM range(0, 2000, 2);")); 
	
	// Pre-check: Cache should be empty (or at least not contain our key)
	auto &cache = ObjectCache::GetObjectCache(client_context);
	REQUIRE(cache.Get<JoinHashTableCacheEntry>("HashJoin_Cache_Test") == nullptr);

	// Run Join
	auto result = con.Query("SELECT COUNT(*) FROM Probes JOIN BuildTable ON Probes.id = BuildTable.id;");
	REQUIRE(CHECK_COLUMN(result, 0, {500}));

	// Post-check: Cache should now contain our entry
	auto cached_entry = cache.Get<JoinHashTableCacheEntry>("HashJoin_Cache_Test");
	REQUIRE(cached_entry != nullptr);
	REQUIRE(cached_entry->hash_table != nullptr);
	REQUIRE( cached_entry->hash_table->Count() == 1000 );
	
	// SECOND QUERY: Should reuse the cached hash table
	// We use the same join condition, so it should hit the cache.
	auto result2 = con.Query("SELECT COUNT(*) FROM BuildTable JOIN Probes ON BuildTable.id = Probes.id;");
	REQUIRE_NO_FAIL(*result2);
	REQUIRE( result2->GetValue(0, 0).GetValue<int64_t>() == 1000 );

	// Verify that the hash table count is still 1000 (didn't get reset or double-built)
	REQUIRE( cached_entry->hash_table->Count() == 1000 );
}
