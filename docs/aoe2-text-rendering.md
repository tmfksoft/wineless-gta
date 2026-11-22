# AoE2 HD GDI text rendering (parked)

Age of Empires II HD draws its UI text with GDI onto a Direct3D surface
(`IDirect3DSurface9::GetDC` → our `gdi_memory_dc`, format `GDI_FMT_XRGB8888`),
then composites that surface with alpha and a per-element colour tint (white
glyphs become blue headings / yellow items / white titles; a black pass is the
drop shadow). So the surface's **alpha channel must carry glyph coverage** for the
composite to work — GDI itself is alpha-unaware.

## Current state (committed, `src/win32/gdi.c`)

`px_put` for `GDI_FMT_XRGB8888` carries the drawn pixel's **luminance** into alpha
(commit ad0783e, reverting the coverage attempts). Result: readable, correctly
coloured text, but:
- thin anti-aliased **white** text (e.g. the "Validating Subscriptions" header)
  looks faint / hollow / outlined;
- the black **drop shadow** is missing (black → luminance 0 → alpha 0).

## Why the header looks corrupted but the body is fine

Alpha is derived from *brightness*, which double-applies anti-alias coverage.
For an AA edge at coverage `c`, `draw_run` first blends the glyph colour toward
the black background (white → grey `(c,c,c)`), then alpha = luminance of that grey
= `c`. The composite applies coverage again, so the pixel contributes ~`c²`
instead of `c`.
- The **header** is a thin ornate serif — mostly partial-coverage strokes, so the
  double-dim makes it faint/hollow.
- The **body** is bold (thick full-coverage interiors, `c=255`, double-dim is a
  no-op) and saturated (dimming hits one channel), so it stays solid.

So the real axis is **thin anti-aliased strokes vs thick solid ones**, exposed by
alpha coming from brightness rather than coverage.

## The correct fix

Premultiplied coverage alpha in `text_px` for `GDI_FMT_XRGB8888`:
`rgb = text_colour × coverage / 255`, `alpha = coverage`. This applies coverage
once and decouples alpha from brightness, so thin white strokes and bold colour
both render solid. (A plain non-premultiplied `(colour, coverage)` looked like a
colored outline — the game's blend is premultiplied.)

**Caveat that made premultiplied look wrong before:** it also restores the black
drop-shadow pass (which luminance hid via alpha 0), and that looked heavy. Before
committing premultiplied, confirm the game's actual composite blend rather than
assuming it:
- add a temporary log/override of `IDirect3DDevice9::SetRenderState` for
  `D3DRS_SRCBLEND`(19)/`D3DRS_DESTBLEND`(20)/`D3DRS_ALPHABLENDENABLE`(27) and the
  stage-0 `D3DTSS_COLOROP`/`COLORARG`/`ALPHAOP` around when the text-surface
  texture is bound (`SetTexture`), to see whether it's premultiplied
  (`SRCBLEND=ONE`) or straight (`SRCBLEND=SRCALPHA`) and how the shadow pass is
  drawn. Then match the surface contents to that blend so the shadow is subtle,
  not heavy.

Only the `gdi_memory_dc` (d3d9 `GetDC`) path is affected; opaque X8R8G8B8 surfaces
ignore alpha, and Tomb Raider III uses the separate DirectDraw surface-DC path.
Verified GTA SA and TR3 are unaffected by the alpha changes.
