package app

import (
	_ "embed"
	"fmt"
	"image"
	"image/color"
	"math"
	"strings"

	"gioui.org/op"
	xfont "golang.org/x/image/font"
	"golang.org/x/image/font/sfnt"
	"golang.org/x/image/math/fixed"

	"gioui.org/font"
	"gioui.org/font/opentype"
	"golang.org/x/image/font/gofont/gomono"
	"golang.org/x/image/font/gofont/gomonobold"

	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget/material"
)

// Palette is one of the firmware's own themes, ported token for token
// from core/ui/palette.c (PAL_LINEN, PAL_ONYX) and widened from the
// device's RGB565 to 24-bit: the #rrggbb comment in palette.c where it
// gives one, the RGB565 value expanded where it does not. Using the
// device's own colours is not decoration: the app and the iPod are the
// same product, and a host tool in Material blue would look like
// somebody else's.
//
// The app is monochrome the way the device is: ink on surface, and the
// primary button is the iPod's selected row (an Ink bar with Surface
// text). Accent — terracotta — has the single job it has on the device:
// a warning. Untested hardware, a raw disk that needs Administrator, a
// job that failed. A screen where four things are orange is a screen
// where nothing is.
type Palette struct {
	Surface color.NRGBA // page ground                       PAL_SURFACE
	Ink     color.NRGBA // primary text, the selected bar    PAL_INK
	Muted   color.NRGBA // secondary text                    PAL_MUTED
	Muted2  color.NRGBA // tertiary text, hints              PAL_MUTED2
	MutedD  color.NRGBA // strong secondary                  PAL_MUTED_D
	Accent  color.NRGBA // warnings only                     PAL_ACCENT
	Border  color.NRGBA // hairlines                         PAL_BORDER
	Plate   color.NRGBA // card ground                       PAL_PLATE
	Trk     color.NRGBA // progress track, dividers          PAL_TRK
	SelSub  color.NRGBA // disabled text                     PAL_SEL_SUB
	SelTrk  color.NRGBA // log pane text                     PAL_SEL_TRK
	PillOff color.NRGBA // disabled button ground            PAL_PILL_OFF

	// Scrim is what a modal dims the window with. It is not a device
	// token (the iPod has no modals over a live screen): it is the
	// darker end of the palette at 60 %, so Onyx's dims with its
	// charcoal rather than with its off-white ink.
	Scrim color.NRGBA
}

// LinenPalette is PAL_LINEN: warm off-white, near-black ink.
func LinenPalette() Palette {
	p := Palette{
		Surface: rgb(0xF4F1EC), // 0xF79D  #f4f1ec
		Ink:     rgb(0x1A1714), // 0x18A2  #1a1714
		Muted:   rgb(0x7A7068), // 0x7B8D  #7a7068
		Muted2:  rgb(0x9A8E80), // 0x9C70  #9a8e80
		MutedD:  rgb(0x5A5048), // 0x5A89  #5a5048
		Accent:  rgb565(0xC348),
		Border:  rgb565(0xE71B),
		Plate:   rgb565(0xF7BE),
		Trk:     rgb565(0xDEDA),
		SelSub:  rgb565(0xB595),
		SelTrk:  rgb565(0x41E7),
		PillOff: rgb565(0xCE58),
	}
	p.Scrim = withAlpha(p.Ink, 0x99)
	return p
}

// OnyxPalette is PAL_ONYX: Linen's ink and surface swapped and warmed,
// so the selected bar becomes a light bar with dark text — the same
// inversion Linen has, the other way up.
func OnyxPalette() Palette {
	p := Palette{
		Surface: rgb(0x1C1A17), // 0x18C2  #1c1a17
		Ink:     rgb(0xECE7E0), // 0xEF3C  #ece7e0
		Muted:   rgb(0xA89D90), // 0xACF2  #a89d90
		Muted2:  rgb(0xB0A598), // 0xB533  #b0a598
		MutedD:  rgb(0x8A8075), // 0x8C0E  #8a8075
		Accent:  rgb565(0xC348),
		Border:  rgb(0x35302B), // 0x3185  #35302b
		Plate:   rgb565(0x2944),
		Trk:     rgb(0x3A352F), // 0x39A5  #3a352f
		SelSub:  rgb565(0x5A89),
		SelTrk:  rgb565(0xCE16),
		PillOff: rgb565(0x39A5),
	}
	p.Scrim = withAlpha(rgb(0x0E0D0B), 0xB0)
	return p
}

func rgb(v uint32) color.NRGBA {
	return color.NRGBA{R: uint8(v >> 16), G: uint8(v >> 8), B: uint8(v), A: 0xFF}
}

// rgb565 widens a device colour the way the panel's own expansion does:
// the top bits of each channel repeated into the low ones, so 0x1F is
// 0xFF and not 0xF8.
func rgb565(v uint16) color.NRGBA {
	r := uint8(v>>11)<<3 | uint8(v>>13)
	g := uint8((v>>5)&63)<<2 | uint8((v>>9)&3)
	b := uint8(v&31)<<3 | uint8((v>>2)&7)
	return color.NRGBA{R: r, G: g, B: b, A: 0xFF}
}

func withAlpha(c color.NRGBA, a uint8) color.NRGBA {
	c.A = a
	return c
}

// mix is a + (b-a)·t per channel, opaque: the blend the tile fallbacks
// are made of.
func mix(a, b color.NRGBA, t float32) color.NRGBA {
	l := func(x, y uint8) uint8 { return uint8(float32(x) + (float32(y)-float32(x))*t + 0.5) }
	return color.NRGBA{R: l(a.R, b.R), G: l(a.G, b.G), B: l(a.B, b.B), A: 0xFF}
}

// luma is a cheap perceived brightness, 0..255, for picking which end of
// the palette reads on a given ground.
func luma(c color.NRGBA) int {
	return (299*int(c.R) + 587*int(c.G) + 114*int(c.B)) / 1000
}

// Theme is the palette choice: the one the system implies, or one of the
// two named. It is what config.json stores as "theme"; the empty string
// is System, so a config written before the field existed follows the
// system.
type Theme string

const (
	ThemeSystem Theme = ""
	ThemeLinen  Theme = "linen"
	ThemeOnyx   Theme = "onyx"
)

// ParseTheme reads the --theme flag's spelling (and config.json's).
func ParseTheme(s string) (Theme, error) {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "", "system", "auto":
		return ThemeSystem, nil
	case "linen", "light":
		return ThemeLinen, nil
	case "onyx", "dark":
		return ThemeOnyx, nil
	}
	return ThemeSystem, fmt.Errorf("theme %q: want system, linen or onyx", s)
}

// String is the word a menu would show.
func (t Theme) String() string {
	switch t {
	case ThemeLinen:
		return "Linen"
	case ThemeOnyx:
		return "Onyx"
	}
	return "System"
}

// Resolve turns System into the palette the OS asks for: Onyx when the
// system is dark (systemDark, per OS), Linen otherwise. An unknown
// spelling — a config from a newer build — is System too.
func (t Theme) Resolve() Theme {
	switch t {
	case ThemeLinen, ThemeOnyx:
		return t
	}
	if systemDark() {
		return ThemeOnyx
	}
	return ThemeLinen
}

// Palette is the resolved choice's colours.
func (t Theme) Palette() Palette {
	if t.Resolve() == ThemeOnyx {
		return OnyxPalette()
	}
	return LinenPalette()
}

// Nunito is the device's typeface (tools/fonts-src, the same files the
// firmware's atlases are rasterised from), shipped inside the binary
// with its licence beside it in fonts/. Mono is the log's: the log
// carries device paths, byte counts and checksums, and a proportional
// font turns a column of those into a ragged wall.
const Nunito font.Typeface = "Nunito"

var Mono = font.Font{Typeface: "Go Mono"}

var (
	//go:embed fonts/Nunito-Regular.ttf
	nunitoRegular []byte
	//go:embed fonts/Nunito-SemiBold.ttf
	nunitoSemiBold []byte
	//go:embed fonts/Nunito-Bold.ttf
	nunitoBold []byte
)

// fontCollection is the shaper's faces: the three Nunito weights the
// app draws in, and Go Mono (regular and bold) for the log. The weights
// are set here rather than read from the files' OS/2 tables, so a face
// lands where the code asks for it whatever its metadata says.
func fontCollection() []font.FontFace {
	var out []font.FontFace
	add := func(ttf []byte, f font.Font) {
		face, err := opentype.Parse(ttf)
		if err != nil {
			// The bytes are embedded at build time; a parse failure is
			// a broken build, not a runtime condition.
			panic(fmt.Errorf("app: parsing an embedded font: %v", err))
		}
		out = append(out, font.FontFace{Font: f, Face: face})
	}
	add(nunitoRegular, font.Font{Typeface: Nunito, Weight: font.Normal})
	add(nunitoSemiBold, font.Font{Typeface: Nunito, Weight: font.SemiBold})
	add(nunitoBold, font.Font{Typeface: Nunito, Weight: font.Bold})
	add(gomono.TTF, font.Font{Typeface: Mono.Typeface, Weight: font.Normal})
	add(gomonobold.TTF, font.Font{Typeface: Mono.Typeface, Weight: font.Bold})
	return out
}

// newTheme builds the Gio theme. The fonts ship inside the binary, so
// the app has no font dependency on the machine it runs on — which
// matters on a fresh Windows install, where "download one exe and run
// it" is the whole promise.
func newTheme(t Theme) (*material.Theme, Palette) {
	th := material.NewTheme()
	th.Shaper = text.NewShaper(text.WithCollection(fontCollection()))
	th.Face = Nunito
	th.TextSize = unit.Sp(14)
	pal := t.Palette()
	setMaterial(th, pal)
	return th, pal
}

// setMaterial points the material widgets' own colours at the palette:
// Contrast* is what a material.Button or a selection would otherwise
// paint in.
func setMaterial(th *material.Theme, pal Palette) {
	th.Palette = material.Palette{
		Fg:         pal.Ink,
		Bg:         pal.Surface,
		ContrastBg: pal.Ink,
		ContrastFg: pal.Surface,
	}
}

// FocusGained re-reads the system theme when the window comes back to the
// front, so a dark-mode switch made in Windows' settings shows the next time
// the window is used, without a restart. Only for System: a named choice is
// the user's and stays.
func (u *UI) FocusGained() {
	if u.theme == ThemeSystem {
		u.applyTheme(ThemeSystem)
	}
}

// applyTheme switches the window's palette. The shaper is kept — it is
// the expensive half and does not depend on colour — and the next frame
// draws in the new tokens, because every widget reads u.pal per frame.
func (u *UI) applyTheme(t Theme) {
	u.theme = t
	u.pal = t.Palette()
	setMaterial(u.th, u.pal)
	u.invalidate()
}

// --- centring text on a row by its capitals --------------------------------

// nunitoCap and monoCap are the faces' cap heights as a fraction of the
// em: the OS/2 table's sCapHeight, read once from the embedded face, or —
// when a font leaves that field zero — the ink height of "H".
var (
	nunitoCap = capHeight(nunitoRegular)
	monoCap   = capHeight(gomono.TTF)
)

// capHeight is one face's cap height per em.
func capHeight(ttf []byte) float32 {
	f, err := sfnt.Parse(ttf)
	if err != nil {
		return 0.7
	}
	var b sfnt.Buffer
	upem := fixed.Int26_6(f.UnitsPerEm())
	m, err := f.Metrics(&b, upem<<6, xfont.HintingNone)
	if err == nil && m.CapHeight > 0 {
		return float32(m.CapHeight) / float32(upem<<6)
	}
	if gi, err := f.GlyphIndex(&b, 'H'); err == nil {
		if r, _, err := f.GlyphBounds(&b, gi, upem<<6, xfont.HintingNone); err == nil {
			return float32(-r.Min.Y) / float32(upem<<6)
		}
	}
	return 0.7
}

// rowText lays one line of text out so that its CAPITALS are centred
// vertically in a band h px tall — gtx.Constraints.Min.Y when h is 0, and
// never less than the line's own box. A material.Label is a box of
// ascent + descent with its text at the top, and centring that box (or
// letting a Flex hand the label a tall Min.Y, which puts the text at the
// top of it) centres the wrong thing: Nunito's ascender is tall, so the
// letters ride high. Here the baseline goes where the band's middle is
// half a cap height above it. The returned Baseline is the label's, so a
// Baseline-aligned Flex still lines rows of mixed sizes up.
func rowText(gtx C, l material.LabelStyle, h int) D {
	l.MaxLines = 1
	if h <= 0 {
		h = gtx.Constraints.Min.Y
	}
	g := gtx
	g.Constraints.Min.Y = 0
	m := op.Record(gtx.Ops)
	d := l.Layout(g)
	call := m.Stop()
	if h < d.Size.Y {
		h = d.Size.Y
	}
	capPx := nunitoCap * float32(l.TextSize) * gtx.Metric.PxPerSp
	if l.Font.Typeface == Mono.Typeface {
		capPx = monoCap * float32(l.TextSize) * gtx.Metric.PxPerSp
	}
	want := int(math.Round(float64((float32(h) + capPx) / 2)))
	off := want - (d.Size.Y - d.Baseline)
	st := op.Offset(image.Pt(0, off)).Push(gtx.Ops)
	call.Add(gtx.Ops)
	st.Pop()
	w := d.Size.X
	if gtx.Constraints.Min.X > w {
		w = gtx.Constraints.Min.X
	}
	return D{Size: image.Pt(w, h), Baseline: h - want}
}
