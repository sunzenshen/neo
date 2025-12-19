## 2024-05-23 - CUtlMap Performance Anti-Pattern
**Learning:** `CUtlMap::Find` (O(log n)) introduces measurable overhead in high-frequency loops (e.g., bot visibility checks) when the key space is small and bounded (player indices).
**Action:** For player-indexed data, always prefer fixed-size arrays (`type arr[MAX_PLAYERS_ARRAY_SAFE]`) which provide O(1) access. Ensure explicit cache invalidation in `Spawn()`.
