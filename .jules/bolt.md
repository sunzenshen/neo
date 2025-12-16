# Bolt's Journal
## 2024-05-22 - [Optimizing Player Fog Cache]
**Learning:** Replacing  with a fixed-size array for per-player data significantly improves performance by eliminating tree lookups and memory allocation, especially in hot paths like  which is called frequently by bots.
**Action:** When caching data that is indexed by player entity index, always prefer  sized arrays over  or , and validate indices with .
## 2024-05-22 - [Optimizing Player Fog Cache]
**Learning:** Replacing `CUtlMap` with a fixed-size array for per-player data significantly improves performance by eliminating tree lookups and memory allocation, especially in hot paths like `IsHiddenByFog` which is called frequently by bots.
**Action:** When caching data that is indexed by player entity index, always prefer `MAX_PLAYERS_ARRAY_SAFE` sized arrays over `CUtlMap` or `CUtlVector`, and validate indices with `IsIndexIntoPlayerArrayValid`.
