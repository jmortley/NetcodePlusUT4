# Clutch HUD asset sources

This directory keeps the lossless image sources used to recreate the Clutch HUD.

- `hud_top_bottom2.png`, `hud_bottom.png`, and `hud_bottom_Blue.png` are recovered overlays exported losslessly from the original Clutch package.
- `Layers/*.png` are normalized, transparent source layers used for the editor-imported textures under `Content/Clutch/HUD/Textures/Legacy`.
- The native HUD first loads editor assets from the NetcodePlus plugin mount (`/NetcodePlus/Clutch/...`), then checks the historical project path (`/Game/Clutch/...`). The recovered top overlay also has a runtime PNG fallback.

The original cooked package assets are intentionally not stored here as editable assets. Cooked `.uasset` files cannot be safely opened or resaved by the UT4 editor. The `.uasset` files under the plugin's `Content` directory are editor-created imports.
