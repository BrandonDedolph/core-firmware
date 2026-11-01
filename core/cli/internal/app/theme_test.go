package app

import (
	"encoding/json"
	"image/color"
	"strings"
	"testing"
)

// The palettes are palette.c's. The hex comments there are the source where
// they exist; the rest are RGB565 expanded the way the panel does it, so a
// handful of those are pinned here against hand-expanded values.
func TestPalettesArePaletteC(t *testing.T) {
	cases := []struct {
		name string
		got  color.NRGBA
		want uint32
	}{
		{"linen surface", LinenPalette().Surface, 0xF4F1EC},
		{"linen ink", LinenPalette().Ink, 0x1A1714},
		{"linen accent 0xC348", LinenPalette().Accent, 0xC66942},
		{"linen border 0xE71B", LinenPalette().Border, 0xE7E3DE},
		{"linen sel_trk 0x41E7", LinenPalette().SelTrk, 0x423C39},
		{"linen pill_off 0xCE58", LinenPalette().PillOff, 0xCECBC6},
		{"onyx surface", OnyxPalette().Surface, 0x1C1A17},
		{"onyx ink", OnyxPalette().Ink, 0xECE7E0},
		{"onyx plate 0x2944", OnyxPalette().Plate, 0x292821},
		{"onyx sel_trk 0xCE16", OnyxPalette().SelTrk, 0xCEC3B5},
		{"onyx sel_sub 0x5A89", OnyxPalette().SelSub, 0x5A514A},
	}
	for _, c := range cases {
		if c.got != rgb(c.want) {
			t.Errorf("%s = %02X%02X%02X, want %06X", c.name, c.got.R, c.got.G, c.got.B, c.want)
		}
	}
	if rgb565(0xFFFF) != rgb(0xFFFFFF) || rgb565(0) != rgb(0) {
		t.Error("rgb565 does not reach both ends of the range")
	}
}

func TestParseTheme(t *testing.T) {
	for in, want := range map[string]Theme{
		"": ThemeSystem, "system": ThemeSystem, "Linen": ThemeLinen, "onyx": ThemeOnyx, " dark ": ThemeOnyx,
	} {
		got, err := ParseTheme(in)
		if err != nil || got != want {
			t.Errorf("ParseTheme(%q) = %q, %v; want %q", in, got, err, want)
		}
	}
	if _, err := ParseTheme("sage"); err == nil {
		t.Error("ParseTheme accepted a theme the app does not have")
	}
	if ThemeLinen.Resolve() != ThemeLinen || ThemeOnyx.Resolve() != ThemeOnyx {
		t.Error("a named theme resolved to something else")
	}
	if r := ThemeSystem.Resolve(); r != ThemeLinen && r != ThemeOnyx {
		t.Errorf("System resolved to %q", r)
	}
}

// config.json spells the choice "theme", and System is the field left out.
func TestThemeInConfigJSON(t *testing.T) {
	b, _ := json.Marshal(Config{Theme: ThemeOnyx})
	if !strings.Contains(string(b), `"theme":"onyx"`) {
		t.Errorf("config JSON is %s", b)
	}
	b, _ = json.Marshal(Config{})
	if strings.Contains(string(b), "theme") {
		t.Errorf("System should leave the field out: %s", b)
	}
}

// applyTheme swaps the palette the frame draws with and the material
// theme's own colours together.
func TestApplyTheme(t *testing.T) {
	onyx := ThemeOnyx
	u := NewUI(Options{Theme: &onyx})
	if u.pal != OnyxPalette() {
		t.Fatal("Options.Theme did not pick Onyx")
	}
	u.applyTheme(ThemeLinen)
	if u.pal != LinenPalette() || u.th.Palette.Bg != LinenPalette().Surface ||
		u.th.Palette.ContrastBg != LinenPalette().Ink {
		t.Error("applyTheme left part of the window in the old palette")
	}
}

// The tile fallbacks come from the palette: Onyx's are dark, Linen's light,
// and none is the accent.
func TestTileColorsFollowThePalette(t *testing.T) {
	for _, seed := range []string{"Aja", "Voodoo", "Hats", "24", "Dummy", "DOA"} {
		l, o := tileColor(LinenPalette(), seed), tileColor(OnyxPalette(), seed)
		if luma(o) >= luma(l) {
			t.Errorf("%s: Onyx tile %v is not darker than Linen's %v", seed, o, l)
		}
		if luma(o) > 0x80 {
			t.Errorf("%s: Onyx tile %v is a light tile", seed, o)
		}
		if l == LinenPalette().Accent || o == OnyxPalette().Accent {
			t.Errorf("%s: a tile is the accent", seed)
		}
	}
}
