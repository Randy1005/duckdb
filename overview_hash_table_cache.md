# Hash Table Reuse Implementation Plan (Update: Dec 17)

**Objective**: Enable caching of the `JoinHashTable` in `PhysicalHashJoin` to reuse the build side across multiple query executions, avoiding redundant hash table construction.

## Conceptual Overview

- **Current Strategy**: We opted for a "hacky" execution-time modification for a quick demo, rather than a generalized physical plan optimization which would be more complex.

### Use an `ObjectCache` to store the built hash table

We need a place to store the built `JoinHashTable` so it survives beyond the execution of a single query.

- **Problem**: `PhysicalHashJoin` operators are transient; they are created and destroyed for each query.
- **Solution**: The `ObjectCache` is a persistent storage mechanism attached to the `DatabaseInstance`.

## Proposed Changes

### 1. Define Cache Entry

- **File**: `duckdb/execution/operator/join/physical_hash_join.hpp`
- **Class**: `JoinHashTableCacheEntry : public ObjectCacheEntry`

```cpp
// A wrapper to store the Hash Table in DuckDB's cache.
class JoinHashTableCacheEntry : public ObjectCacheEntry {
public:
    static constexpr const char* TYPE_NAME = "JoinHashTableCacheEntry";

    explicit JoinHashTableCacheEntry(shared_ptr<JoinHashTable> ht)
        : hash_table(std::move(ht)) {}

    string GetObjectType() override { return ObjectType(); }
    static string ObjectType() { return TYPE_NAME; }

    shared_ptr<JoinHashTable> hash_table;
};
```

### 2. Hacking The Sink State In PhysicalHashJoin::GetGlobalSinkState

- **File**: `duckdb/execution/operator/join/physical_hash_join.cpp`
- **Method**: `PhysicalHashJoin::GetGlobalSinkState`

**What is a Sink State?**
The `GlobalSinkState` is a **shared container** at the end of a pipeline. Multiple threads "sink" data into it. In a Hash Join, the build-side pipeline ends here.

- **Hash Table Retrieval**: Check `ObjectCache` for "HashJoin_Cache_Test".
- **Hash Table Injection**: If found, assign `gstate.hash_table = cached_entry->hash_table`.

### 3. Short-circuiting Logic to bypass the build pipeline

- **File**: `duckdb/execution/operator/join/physical_hash_join.cpp`
- **Methods**: `PhysicalHashJoin::Sink`, `HashJoinFinalizeEvent::FinishEvent`

- **Sink/Finalize**: Skip actual build tasks if `is_reusable` is true.

---

## Technical Block: Segmentation Fault

To test the implementation, I run two consecutive queries on two tables.
The first query successfully cached the internal hash table, but the second query crashed after retrieving the cached hash table.

```cpp
// ...

// Query 1: Successfully builds and caches the hash table
SELECT count(*) FROM range(1000000) t1(i) JOIN range(1000) t2(j) ON i = j;

// Query 2: Retrieves the cached hash table but triggers SIGSEGV
SELECT count(*) FROM range(1000000) t1(i) JOIN range(1000) t2(j) ON i = j;

// ...

// Debug logs from DuckDB
DEBUG: HashJoinFinalizeEvent::FinishEvent called. Count: 1000
DEBUG: Caching Hash Table... (Count: 1000)
DEBUG: Retrieved Hash Table from cache!
Segmentation fault (core dumped)
```

> [!CAUTION] Query 2 triggers a `SIGSEGV` (Segmentation Fault) despite successful cache retrieval.

### Potential Root Cause Diagnosis

This is likely a **Resource Deallocation** issue:

1. **Memory Pool Release**: Query 1 manages a memory pool for its operators. Upon completion, DuckDB's execution plan triggers a cleanup that might be releasing the underlying memory chunks of the hash table, ignoring the `shared_ptr` count because it thinks it's the primary owner.
2. **Operator Lifecycle**: The cached object still holds pointers to **Operator A** (PhysicalHashJoin instance from Query 1). When Query 2 follows these to perform probing, it hits deleted memory.

---

### Instructions for Reproducing the Segmentation Fault

1. Clone [my duckdb-python repo](https://github.com/Randy1005/duckdb-python.git).
2. Run `git submodule update --init --recursive`, this should grab my custom changes from my fork [here](https://github.com/Randy1005/duckdb/tree/custom-duckdb-changes).
3. Run `make` in the directory `duckdb-python/external/duckdb`, this will build the "hacky persistent hash table cache" and the unit test `duckdb-python/external/duckdb/test/api/test_hash_join_cache.cpp`.
4. In `duckdb-python/external/duckdb`, run `./build/release/test/unittest "[api][hashjoin]"` to see the debug logs confirming cache insertion and retrieval, followed by the segmentation fault.
