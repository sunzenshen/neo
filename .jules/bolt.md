## 2025-12-16 - [Optimizing Map Lookups]
**Learning:** In Source Engine mods, caching data per-player using `CUtlMap` keyed by entity index is an anti-pattern when `MAX_PLAYERS` is small and fixed.
**Action:** Replace `CUtlMap<int, T>` with `T m_Array[MAX_PLAYERS_ARRAY_SAFE]` for O(1) access, ensuring safe bounds checking with `IsIndexIntoPlayerArrayValid`.
