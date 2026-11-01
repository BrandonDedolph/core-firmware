package app

import (
	"fmt"
	"image"
	"os"
	"regexp"
	"strings"

	"gioui.org/f32"
	"gioui.org/font"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget/material"
)

// The iPod tab: this iPod, in five rows a person can read — its name, its
// firmware, how full it is, what it is, and where its backups are — each
// with the one or two things that can be done about it. Everything that is a
// register-level fact (the disk path, the OSOS entry, the device files, the
// image flash) is under Advanced, closed by default. The log is the drawer's,
// as on every screen.

// ipodRow is one row of the plate, as data: what the tests read and what
// the layout draws.
type ipodRow struct {
	label, value, sub string
	// warn is a tail drawn after value in the warning colour
	// ("UNTESTED"); it is empty for everything that is fine.
	warn    string
	actions []btn
}

// ipodPane is the whole tab: the rows, then Advanced.
func (u *UI) ipodPane(gtx C) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D {
			list := material.List(u.th, &u.cards)
			list.Indicator.Color = u.pal.SelSub
			list.Indicator.HoverColor = u.pal.Muted2
			return list.Layout(gtx, 1, func(gtx C, _ int) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.rowsPlate),
					layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(u.advancedToggle),
					layout.Rigid(func(gtx C) D {
						if !u.st.Advanced {
							return D{}
						}
						return layout.Inset{Top: unit.Dp(4)}.Layout(gtx, u.advancedPlate)
					}),
				)
			})
		}),
	)
}

// revealAdvanced scrolls the tab to the bottom so a disclosure that has
// just opened is on screen; the list clamps the offset to its content.
func (u *UI) revealAdvanced() {
	u.cards.Position.Offset = 1 << 20
	u.cards.Position.BeforeEnd = false
}

// ipodRows is the plate's content.
func (u *UI) ipodRows() []ipodRow {
	d, lib, rel, bk := u.st.Device, u.st.Library, u.st.Release, u.st.Backups
	busy := u.st.Busy()
	var rows []ipodRow

	name := strings.TrimSpace(d.Name)
	if name == "" {
		name = "Not named yet"
	}
	rows = append(rows, ipodRow{label: "Name", value: name,
		sub:     "shown on the iPod's boot screen and in Windows",
		actions: []btn{{&u.nameClick, "Rename", false, !busy && d.Volume != ""}}})

	fw := ipodRow{label: "Firmware", value: "Core"}
	if tag := installedTag(d.Firmware); tag != "" {
		fw.value = "Core " + tag
	}
	switch {
	case u.st.FirmwareTooOld():
		fw.sub = TooOldText
		if rel.Checked && rel.Tag != "" && rel.Err == "" {
			fw.actions = []btn{{&u.updateBtn, "Update", true, !busy}}
		} else {
			fw.actions = []btn{{&u.checkBtn, "Check", false, !busy}}
		}
	case u.st.UpdateAvailable():
		fw.sub = rel.Tag + " is available"
		if !rel.Published.IsZero() {
			fw.sub += " · released " + rel.Published.Format("Jan 2, 2006")
		}
		if n := firstNote(rel.Notes, 60); n != "" {
			fw.sub += " · " + n
		}
		fw.actions = []btn{{&u.updateBtn, "Update", true, !busy}}
	case rel.Checked && rel.Err == "" && rel.Tag != "":
		fw.sub = "up to date"
	case rel.Checked && rel.Err != "":
		fw.sub = "could not check — the log says why"
		fw.actions = []btn{{&u.checkBtn, "Check", false, !busy}}
	default:
		fw.sub = "not checked yet — ··· → Check for updates"
		fw.actions = []btn{{&u.checkBtn, "Check", false, !busy}}
	}
	rows = append(rows, fw)

	st := ipodRow{label: "Storage", value: "not read yet"}
	if d.Capacity > 0 {
		fv, fu := humanBinary(d.Free)
		tv, tu := humanBinary(d.Capacity)
		st.value = fv + " " + fu + " free of " + tv + " " + tu
	}
	switch {
	case lib.Present:
		st.sub = fmt.Sprintf("%d songs · %d albums · %d genres on the iPod", lib.Songs, lib.Albums, lib.Genres)
	default:
		st.sub = "no library on the iPod yet — the first sync writes one"
	}
	rows = append(rows, st)

	hw := ipodRow{label: "Hardware", value: modelWords(d.Model) + " · "}
	if d.Tested {
		hw.value += "tested"
	} else {
		hw.warn = "UNTESTED"
	}
	if d.Serial != "" {
		hw.sub = "serial " + d.Serial
	}
	if !d.Tested {
		// The app does not write firmware to it (ErrUntested); say so
		// here rather than only when a write is refused.
		if hw.sub != "" {
			hw.sub += " · "
		}
		hw.sub += "the app will not write firmware to it; the command line can, with --untested-hardware"
	}
	rows = append(rows, hw)

	b := ipodRow{label: "Backups", value: "No Apple firmware backup here"}
	if !bk.Apple.IsZero() {
		b.value = "Apple's firmware, saved " + bk.Apple.Format("Jan 2, 2006")
	}
	b.sub = fmt.Sprintf("and the partition from before each Core write · %d file%s", bk.Count, plural(bk.Count))
	b.actions = []btn{
		{&u.backupDirBtn, "Show folder", false, bk.Dir != ""},
		{&u.backupBtn, "Back up now", false, !busy && d.Found},
	}
	rows = append(rows, b)
	return rows
}

// modelRE splits "iPod Video 5.5G 80 GB" into the model and its size.
var modelRE = regexp.MustCompile(`^(.*\S) (\d+ ?[GT]B)$`)

// modelWords is the model as the Hardware row says it: "iPod Video 5.5G,
// 80 GB".
func modelWords(m string) string {
	m = strings.TrimSpace(m)
	if m == "" {
		return DefaultDeviceName
	}
	if g := modelRE.FindStringSubmatch(m); g != nil {
		return g[1] + ", " + g[2]
	}
	return m
}

// firstNote is the first real line of a release's notes, without its
// markdown, cut to n runes.
func firstNote(notes string, n int) string {
	for _, line := range strings.Split(notes, "\n") {
		line = strings.TrimSpace(strings.TrimLeft(strings.TrimSpace(line), "#-*> "))
		if line == "" {
			continue
		}
		if r := []rune(line); len(r) > n {
			return strings.TrimSpace(string(r[:n-1])) + "…"
		}
		return line
	}
	return ""
}

// rowsPlate draws the rows on one plate, hairlines between them.
func (u *UI) rowsPlate(gtx C) D {
	rows := u.ipodRows()
	return u.plateR(gtx, 7, func(gtx C) D {
		var children []layout.FlexChild
		for i := range rows {
			r := rows[i]
			if i > 0 {
				children = append(children, layout.Rigid(u.hairline))
			}
			children = append(children, layout.Rigid(func(gtx C) D {
				if r.label == "Name" && u.editingName {
					return u.rowFrame(gtx, r.label, u.nameEditor, nil)
				}
				return u.rowFrame(gtx, r.label, func(gtx C) D { return u.rowValue(gtx, r) }, r.actions)
			}))
		}
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx, children...)
	})
}

// rowFrame is the row's three columns: label, body, actions.
func (u *UI) rowFrame(gtx C, label string, body layout.Widget, actions []btn) D {
	return layout.Inset{Top: unit.Dp(8), Bottom: unit.Dp(8), Left: unit.Dp(14), Right: unit.Dp(12)}.
		Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			w := gtx.Dp(150)
			// The label is laid out after the row, over its first 150 dp,
			// so it knows the row's height and can centre its capitals on
			// it (rowText) rather than its line box.
			return layout.Stack{}.Layout(gtx,
				layout.Stacked(func(gtx C) D {
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
						layout.Rigid(layout.Spacer{Width: unit.Dp(150)}.Layout),
						layout.Flexed(1, body),
						layout.Rigid(func(gtx C) D {
							if len(actions) == 0 {
								return D{}
							}
							return layout.Inset{Left: unit.Dp(8)}.Layout(gtx, func(gtx C) D {
								return u.buttonRow(gtx, actions...)
							})
						}),
					)
				}),
				layout.Expanded(func(gtx C) D {
					gtx.Constraints.Min.X, gtx.Constraints.Max.X = w, w
					l := material.Label(u.th, unit.Sp(12.5), label)
					l.Font.Weight = font.SemiBold
					l.Color = u.pal.Muted
					return rowText(gtx, l, gtx.Constraints.Min.Y)
				}),
			)
		})
}

// rowValue is the value line (with its warning tail) and the sub line.
func (u *UI) rowValue(gtx C, r ipodRow) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
				layout.Rigid(u.line(13, u.pal.Ink, r.value)),
				layout.Rigid(func(gtx C) D {
					if r.warn == "" {
						return D{}
					}
					l := material.Label(u.th, unit.Sp(13), r.warn)
					l.Font.Weight = font.Bold
					l.Color = u.pal.Accent
					return l.Layout(gtx)
				}),
			)
		}),
		layout.Rigid(func(gtx C) D {
			if r.sub == "" {
				return D{}
			}
			return u.line(11, u.pal.Muted2, r.sub)(gtx)
		}),
	)
}

// nameEditor is the Name row while it is being renamed: the field, Save and
// Cancel, and the sentence that says what Windows will show.
func (u *UI) nameEditor(gtx C) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
				layout.Flexed(1, func(gtx C) D {
					return u.field(gtx, &u.nameEd, "Brandon's iPod")
				}),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D {
					return u.button(gtx, btn{&u.nameSaveBtn, "Save", true, !u.st.Busy()})
				}),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D {
					return u.button(gtx, btn{&u.nameCancelBtn, "Cancel", false, true})
				}),
			)
		}),
		layout.Rigid(layout.Spacer{Height: unit.Dp(3)}.Layout),
		layout.Rigid(u.line(11, u.pal.Muted2, u.namePreview())),
	)
}

// --- Advanced ------------------------------------------------------------

// advancedHint is the line beside the closed disclosure: what is in it.
const advancedHint = "disk path, partition entry, index and config files, flash an image file"

// advancedToggle is the disclosure row.
func (u *UI) advancedToggle(gtx C) D {
	return u.advancedBtn.Layout(gtx, func(gtx C) D {
		return layout.Stack{}.Layout(gtx,
			layout.Expanded(func(gtx C) D {
				defer clip.Rect{Max: gtx.Constraints.Min}.Push(gtx.Ops).Pop()
				pointer.CursorPointer.Add(gtx.Ops)
				return D{Size: gtx.Constraints.Min}
			}),
			layout.Stacked(func(gtx C) D {
				return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(6), Left: unit.Dp(4)}.Layout(gtx, func(gtx C) D {
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
						layout.Rigid(func(gtx C) D { return u.triangle(gtx, u.st.Advanced) }),
						layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
						layout.Rigid(func(gtx C) D {
							l := material.Label(u.th, unit.Sp(13), "Advanced")
							l.Font.Weight = font.SemiBold
							l.Color = u.pal.Ink
							return l.Layout(gtx)
						}),
						layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
						layout.Rigid(u.text(11, u.pal.Muted2, advancedHint)),
					)
				})
			}),
		)
	})
}

// triangle is the disclosure mark: pointing right when closed, down when
// open, 8 dp, ink.
func (u *UI) triangle(gtx C, open bool) D {
	s := float32(gtx.Dp(8))
	var p clip.Path
	p.Begin(gtx.Ops)
	if open {
		p.MoveTo(f32.Pt(0, s*0.2))
		p.LineTo(f32.Pt(s, s*0.2))
		p.LineTo(f32.Pt(s/2, s*0.9))
	} else {
		p.MoveTo(f32.Pt(s*0.2, 0))
		p.LineTo(f32.Pt(s*0.9, s/2))
		p.LineTo(f32.Pt(s*0.2, s))
	}
	p.Close()
	paint.FillShape(gtx.Ops, u.pal.Ink, clip.Outline{Path: p.End()}.Op())
	sz := gtx.Dp(8)
	return D{Size: image.Pt(sz, sz)}
}

// advFact is one line of the Advanced plate. mono is for paths and hex.
type advFact struct {
	label, value string
	mono, warn   bool
}

// advancedFacts is everything the old THIS IPOD, FIRMWARE and MUSIC FOLDER
// cards said that the rows above do not.
func (u *UI) advancedFacts() []advFact {
	d, lib := u.st.Device, u.st.Library
	f := []advFact{
		{"Disk", d.Path, true, false},
		{"Volume", d.Volume, true, false},
		{"Sector size", itoa(d.SectorSize) + " B", false, false},
	}
	osos := okWord(d.OSOSOK)
	if d.OSOSNote != "" {
		osos += " — " + d.OSOSNote
	}
	f = append(f, advFact{"OSOS entry", osos, true, !d.OSOSOK})
	if d.Firmware != "" {
		f = append(f, advFact{"Installed build", d.Firmware, true, false})
	}
	switch {
	case lib.Present:
		f = append(f, advFact{"Device files", "CORELIB.IDX " + comma(lib.IndexBytes) + " B  ·  CORECFG.DAT " +
			okWord(lib.ConfigValid) + "  ·  CORELOG.BIN " + okWord(lib.LogValid), true, false})
	case lib.Note != "":
		f = append(f, advFact{"Device files", lib.Note, false, false})
	}
	if d.Err != "" {
		f = append(f, advFact{"Last error", d.Err, false, true})
	}
	cli := u.st.CLIPath
	if cli == "" {
		cli = "not found beside this app"
	}
	f = append(f, advFact{"core CLI", cli, true, false})
	if u.st.Backups.Dir != "" {
		f = append(f, advFact{"Backup folder", u.st.Backups.Dir, true, false})
	}
	return f
}

// advancedPlate is the open disclosure.
func (u *UI) advancedPlate(gtx C) D {
	busy := u.st.Busy()
	facts := u.advancedFacts()
	return u.plateR(gtx, 7, func(gtx C) D {
		return layout.UniformInset(unit.Dp(14)).Layout(gtx, func(gtx C) D {
			var rows []layout.FlexChild
			for i := range facts {
				fa := facts[i]
				rows = append(rows, layout.Rigid(func(gtx C) D { return u.factLine(gtx, fa) }))
			}
			rows = append(rows,
				layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
				layout.Rigid(func(gtx C) D {
					return u.factFrame(gtx, "Music", func(gtx C) D {
						ready := !busy && u.st.Device.Volume != ""
						return u.buttonRow(gtx,
							btn{&u.dryRunBtn, "Preview sync", false, ready},
							btn{&u.pruneBtn, "Sync and remove…", false, ready},
						)
					})
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
				layout.Rigid(func(gtx C) D {
					return u.factFrame(gtx, "Flash an image", func(gtx C) D {
						return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
							layout.Rigid(func(gtx C) D {
								return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
									layout.Flexed(1, func(gtx C) D {
										return u.field(gtx, &u.flashEd, "path to core.ipod or core.bin")
									}),
									layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
									layout.Rigid(func(gtx C) D {
										return u.buttonRow(gtx,
											btn{&u.flashOpenBtn, "Choose an image…", false, !busy && u.pickerOK},
											btn{&u.flashBtn, "Flash this image", false,
												!busy && strings.TrimSpace(u.flashEd.Text()) != ""},
										)
									}),
								)
							}),
							layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
							layout.Rigid(u.wrap(11, u.pal.Muted2, u.flashNote())),
						)
					})
				}),
			)
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		})
	})
}

// factFrame is a label column and a body, the Advanced plate's grid.
func (u *UI) factFrame(gtx C, label string, body layout.Widget) D {
	return layout.Inset{Bottom: unit.Dp(3)}.Layout(gtx, func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Constraints.Max.X
		return layout.Flex{Alignment: layout.Start}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				w := gtx.Dp(150)
				gtx.Constraints.Min.X, gtx.Constraints.Max.X = w, w
				return u.line(11.5, u.pal.Muted, label)(gtx)
			}),
			layout.Flexed(1, body),
		)
	})
}

// factLine is one fact.
func (u *UI) factLine(gtx C, fa advFact) D {
	return u.factFrame(gtx, fa.label, func(gtx C) D {
		col := u.pal.Ink
		if fa.warn {
			col = u.pal.Accent
		}
		l := material.Label(u.th, unit.Sp(11.5), fa.value)
		if fa.mono {
			l.Font = Mono
			l.TextSize = unit.Sp(11)
		}
		l.Color = col
		l.MaxLines = 1
		l.Truncator = " "
		return l.Layout(gtx)
	})
}

// ipodStrings is every string the tab shows, Advanced included only when it
// is open — what the forbidden-word test reads.
func (u *UI) ipodStrings() []string {
	var out []string
	for _, r := range u.ipodRows() {
		out = append(out, r.label, r.value, r.sub, r.warn)
		for _, b := range r.actions {
			out = append(out, b.label)
		}
	}
	out = append(out, "Advanced", advancedHint)
	if u.st.Advanced {
		for _, f := range u.advancedFacts() {
			out = append(out, f.label, f.value)
		}
		out = append(out, u.flashNote())
	}
	return out
}

// openBackupFolder shows the backup folder in the file manager, creating it
// first: Explorer pointed at a folder that does not exist opens Documents,
// which is a lie about where the backups go.
func (u *UI) openBackupFolder() {
	dir := u.st.Backups.Dir
	if dir == "" {
		return
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		u.st.Logf("could not create %s: %v", dir, err)
		return
	}
	if err := openFolder(dir); err != nil {
		u.st.Logf("could not open %s: %v", dir, err)
	}
}

// sourceRow is the music folder field and its Browse button, on the Library
// tab's header — the one place the folder is set. The footer's folder text
// comes here (and focuses the field); the empty grid's button and Browse
// both run chooseMusicFolder.
func (u *UI) sourceRow(gtx C, busy bool) D {
	if u.focusSource {
		u.focusSource = false
		gtx.Execute(key.FocusCmd{Tag: &u.sourceEd})
	}
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D {
			return u.field(gtx, &u.sourceEd, `C:\Users\you\Music`)
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
		layout.Rigid(func(gtx C) D {
			return u.button(gtx, btn{&u.browseBtn, "Browse…", false, !busy && u.pickerOK})
		}),
	)
}

// chooseMusicFolder is the folder picker, from wherever it is offered: the
// Library tab's Browse and the empty grid's button. The choice lands in
// setSource through drain, like every picker result.
func (u *UI) chooseMusicFolder() { u.pick(JobSync) }

// footerSource is the bottom strip's left half: the music folder, which the
// mockup keeps in the footer with a Change beside it.
func (u *UI) footerSource() string {
	if src := strings.TrimSpace(u.st.Source); src != "" {
		return "Music folder: " + src
	}
	return "No music folder chosen"
}
