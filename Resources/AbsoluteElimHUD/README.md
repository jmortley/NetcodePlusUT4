# Absolute Elim HUD resources

These PNGs are the recovered Elimination 1.13 HUD and scoreboard artwork.
`NCPlusHUDLayout.cpp` loads the top panel and `ElimPlusScoreboard.cpp` loads the
scoreboard skin as loose plugin resources at runtime.

- Name plates: 210 x 70
- Team score plates: 90 x 90
- Health and armor icons: 12 x 12
- Team scoreboard banners: 960 x 140
- Player scoreboard rows: 830 x 64
- Scoreboard category bar: 800 x 30 (mirrored for the red side)
- Authored layout reference: 2560 x 1440

The recovered package contains red name/score plates. The original blue material
variant is reproduced at load time by exchanging the decoded red and blue
channels. This intentionally produces fixed red/blue artwork; the ncHUD custom
Team Color option does not apply while **Absolute Elim 113 layout** is selected.
The scoreboard's dead and totals row appearances are derived at load time from
the recovered row textures using the original material saturation/multiply values.
