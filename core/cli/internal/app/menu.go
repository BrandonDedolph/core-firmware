package app

import (
	"image"
	"io"
	"os/exec"
	"runtime"
	"strings"

	"gioui.org/font"
	"gioui.org/io/clipboard"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

// The "···" menu: everything the header has no room for and nobody needs
// every day. Gio has no menu widget, so this is ours: a plate hung under the
// button, over a click-catcher that covers the window (a click outside
// closes it, as Esc does), drawn in the same top layer as the dialogs.

// The items, in the order they are drawn.
const (
	menuBackup = iota
	menuFlashFile
	menuReread
	menuCheck
	menuCopyLog
	menuOpenMusic
	menuAbout
	menuCount
)

// menuLabels are the words, verb first.
var menuLabels = [menuCount]string{
	menuBackup:    "Back up the firmware",
	menuFlashFile: "Flash an image file…",
	menuReread:    "Read the iPod again",
	menuCheck:     "Check for updates",
	menuCopyLog:   "Copy the log",
	menuOpenMusic: "Open the music folder",
	menuAbout:     "About",
}

// menuGroups puts a hairline between the firmware writes, the reads, the
// clipboard and folder pair, the theme and About.
var menuGroups = [][]int{
	{menuBackup, menuFlashFile},
	{menuReread, menuCheck},
	{menuCopyLog, menuOpenMusic},
	nil, // the theme
	{menuAbout},
}

// themeChoices are the radio rows under "Theme".
var themeChoices = [3]Theme{ThemeSystem, ThemeLinen, ThemeOnyx}

// menuEnabled is each item's condition. A greyed item says the thing
// exists and is not available now, which is more use than an item that
// disappears while a job runs.
func (u *UI) menuEnabled(i int) bool {
	busy := u.st.Busy()
	switch i {
	case menuBackup:
		return !busy && u.st.Device.Found
	case menuFlashFile:
		return !busy && u.st.Device.Found && u.pickerOK
	case menuReread, menuCheck:
		return !busy
	case menuCopyLog:
		return len(u.st.Log) > 0
	case menuOpenMusic:
		return strings.TrimSpace(u.st.Source) != ""
	}
	return true
}

// menuEvents is the menu's half of the frame.
func (u *UI) menuEvents(gtx C) {
	if !u.st.MenuOpen {
		return
	}
	if u.menuScrim.Clicked(gtx) {
		u.st.MenuOpen = false
		return
	}
	for {
		ev, ok := gtx.Event(key.Filter{Name: key.NameEscape})
		if !ok {
			break
		}
		if ke, ok := ev.(key.Event); ok && ke.State == key.Press {
			u.st.MenuOpen = false
			return
		}
	}
	for i := range u.themeClicks {
		if u.themeClicks[i].Clicked(gtx) {
			u.setTheme(themeChoices[i])
		}
	}
	for i := range u.menuClicks {
		if !u.menuClicks[i].Clicked(gtx) || !u.menuEnabled(i) {
			continue
		}
		u.st.MenuOpen = false
		u.menuDo(gtx, i)
		return
	}
}

// menuDo runs one item.
func (u *UI) menuDo(gtx C, i int) {
	switch i {
	case menuBackup:
		u.startBackup()
	case menuFlashFile:
		u.pick(JobFlash)
	case menuReread:
		u.startRefresh()
	case menuCheck:
		u.startCheck()
	case menuCopyLog:
		u.copyLog(gtx)
	case menuOpenMusic:
		u.openMusicFolder()
	case menuAbout:
		u.showAbout()
	}
}

// setTheme applies a palette choice now and remembers it.
func (u *UI) setTheme(t Theme) {
	u.applyTheme(t)
	u.cfg.Theme = t
	u.saveConfig()
}

// copyLog puts the whole log on the clipboard.
func (u *UI) copyLog(gtx C) {
	text := strings.Join(u.st.Log, "\n") + "\n"
	gtx.Execute(clipboard.WriteCmd{Type: "application/text", Data: io.NopCloser(strings.NewReader(text))})
	u.st.Logf("copied %d log lines to the clipboard", len(u.st.Log))
}

// openFolder is the OS file manager on a folder. A variable so a test can
// see what would have been opened without opening anything.
var openFolder = func(dir string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("explorer", dir)
	case "darwin":
		cmd = exec.Command("open", dir)
	default:
		cmd = exec.Command("xdg-open", dir)
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	// Reap it: explorer exits 1 even when it worked, so the status is
	// not worth reading.
	go func() { _ = cmd.Wait() }()
	return nil
}

// openMusicFolder shows the music folder in the file manager, or does
// nothing when there is none.
func (u *UI) openMusicFolder() {
	src := strings.TrimSpace(u.st.Source)
	if src == "" {
		return
	}
	if err := openFolder(src); err != nil {
		u.st.Logf("could not open %s: %v", src, err)
	}
}

// showAbout is the About dialog: a read, closed with its one button.
func (u *UI) showAbout() {
	cli := u.st.CLIPath
	if cli == "" {
		cli = "not found beside this app"
	}
	u.dlg = &dialogRequest{
		title: "About Core",
		body: strings.Join([]string{
			"core-app " + version.Version,
			"commit " + version.Commit + "  ·  built " + version.Date,
			"core CLI: " + cli,
			"",
			"Core is Apache-2.0 · Nunito is OFL",
		}, "\n"),
		okLabel: "Close",
		info:    true,
		reply:   make(chan string, 1),
	}
}

// menuLayer draws the open menu: the click-catcher over the whole window,
// then the plate hung from the right of the top bar.
func (u *UI) menuLayer(gtx C) D {
	full := gtx.Constraints.Max
	u.menuScrim.Layout(gtx, func(gtx C) D { return D{Size: full} })

	w := gtx.Dp(236)
	x := full.X - gtx.Dp(14) - w
	y := u.topH - gtx.Dp(6)
	if y < 0 {
		y = 0
	}
	defer op.Offset(image.Pt(x, y)).Push(gtx.Ops).Pop()
	gtx.Constraints = layout.Constraints{Min: image.Pt(w, 0), Max: image.Pt(w, full.Y-y)}
	return u.plate(gtx, func(gtx C) D {
		return layout.Inset{Top: unit.Dp(5), Bottom: unit.Dp(5)}.Layout(gtx, func(gtx C) D {
			var rows []layout.FlexChild
			for g, items := range menuGroups {
				if g > 0 {
					rows = append(rows, layout.Rigid(func(gtx C) D {
						return layout.Inset{Top: unit.Dp(4), Bottom: unit.Dp(4)}.Layout(gtx, u.hairline)
					}))
				}
				if items == nil {
					rows = append(rows, layout.Rigid(u.themeRows))
					continue
				}
				for _, i := range items {
					i := i
					rows = append(rows, layout.Rigid(func(gtx C) D {
						return u.menuRow(gtx, &u.menuClicks[i], menuLabels[i], u.menuEnabled(i), false, false)
					}))
				}
			}
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		})
	})
}

// themeRows is "Theme" and its three radio rows.
func (u *UI) themeRows(gtx C) D {
	rows := []layout.FlexChild{layout.Rigid(func(gtx C) D {
		return layout.Inset{Top: unit.Dp(4), Bottom: unit.Dp(2), Left: unit.Dp(12)}.
			Layout(gtx, u.line(11, u.pal.Muted2, "Theme"))
	})}
	for i, t := range themeChoices {
		i, t := i, t
		label := t.String()
		if t == ThemeSystem {
			if u.systemNow == "" {
				u.systemNow = ThemeSystem.Resolve()
			}
			label = "System — " + u.systemNow.String() + " now"
		}
		rows = append(rows, layout.Rigid(func(gtx C) D {
			return u.menuRow(gtx, &u.themeClicks[i], label, true, true, u.theme == t)
		}))
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
}

// menuRow is one clickable line. radio rows carry a dot, filled for the
// choice in force.
func (u *UI) menuRow(gtx C, click *widget.Clickable, label string, enabled, radio, on bool) D {
	if !enabled {
		gtx = gtx.Disabled()
	}
	col := u.pal.Ink
	if !enabled {
		col = u.pal.Muted2
	}
	return click.Layout(gtx, func(gtx C) D {
		return layout.Stack{}.Layout(gtx,
			layout.Expanded(func(gtx C) D {
				sz := gtx.Constraints.Min
				if enabled {
					defer clip.Rect{Max: sz}.Push(gtx.Ops).Pop()
					pointer.CursorPointer.Add(gtx.Ops)
					if click.Hovered() {
						paint.FillShape(gtx.Ops, u.pal.Trk, clip.Rect{Max: sz}.Op())
					}
				}
				return D{Size: sz}
			}),
			layout.Stacked(func(gtx C) D {
				gtx.Constraints.Min.X = gtx.Constraints.Max.X
				return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(6), Left: unit.Dp(12), Right: unit.Dp(12)}.
					Layout(gtx, func(gtx C) D {
						return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
							layout.Rigid(func(gtx C) D {
								if !radio {
									return D{}
								}
								return layout.Inset{Right: unit.Dp(8)}.Layout(gtx, func(gtx C) D {
									return u.radioDot(gtx, on)
								})
							}),
							layout.Rigid(func(gtx C) D {
								l := material.Label(u.th, unit.Sp(13), label)
								l.Color = col
								l.MaxLines = 1
								if on {
									l.Font.Weight = font.SemiBold
								}
								return l.Layout(gtx)
							}),
						)
					})
			}),
		)
	})
}

// radioDot is a 10 dp ring, with an ink centre when on.
func (u *UI) radioDot(gtx C, on bool) D {
	s := gtx.Dp(10)
	sz := image.Pt(s, s)
	paint.FillShape(gtx.Ops, u.pal.Muted2, clip.Ellipse{Max: sz}.Op(gtx.Ops))
	w := gtx.Dp(1)
	paint.FillShape(gtx.Ops, u.pal.Plate, clip.Ellipse{Min: image.Pt(w, w), Max: sz.Sub(image.Pt(w, w))}.Op(gtx.Ops))
	if on {
		i := gtx.Dp(2.5)
		paint.FillShape(gtx.Ops, u.pal.Ink, clip.Ellipse{Min: image.Pt(i, i), Max: sz.Sub(image.Pt(i, i))}.Op(gtx.Ops))
	}
	return D{Size: sz}
}
