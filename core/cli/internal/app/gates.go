package app

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// The gates: what stands between a click and a destructive write.
//
// A write to the firmware partition (Flash, Update, Install) is confirmed in
// one dialog that says WHICH iPod, in words a person can check against the
// thing in their hand — name, model, serial, which disk on this PC, how much
// is written — and then asks for a deliberate gesture: a two-second
// press-and-hold. On hardware Core has booted on (Device.Tested), the hold
// IS the confirmation, and the app answers the flasher's typed-path prompt
// with the path it already identified — after checking that the flasher is
// asking about that same path. On anything else the typed path stays.
//
// Sync and remove lists the albums it would delete and says how many on its
// button. No typed word: the list is the confirmation.

// HoldDuration is how long the write button must be held.
const HoldDuration = 2 * time.Second

// writeGate is what a write job reads from the model on the UI goroutine,
// before it starts, for the dialog it will raise on its own goroutine.
type writeGate struct {
	title string
	dev   Device
	cli   string
	image string
}

// gate snapshots the model for one write.
func (u *UI) gate(title string) writeGate {
	return writeGate{title: title, dev: u.st.Device, cli: u.st.CLIPath}
}

// cardRow is one label/value line of the identity card (or one album of
// the removal list).
type cardRow struct{ label, value string }

// identityCard is the five lines that say which iPod this is.
func identityCard(d Device, imageBytes int64) []cardRow {
	name := strings.TrimSpace(d.Name)
	if name == "" {
		name = modelWords(d.Model)
	}
	writes := "the firmware image"
	if imageBytes > 0 {
		v, unit := humanBinary(imageBytes)
		writes = v + " " + unit + " of firmware"
	}
	rows := []cardRow{
		{"Name", name},
		{"Model", modelWords(d.Model)},
	}
	if d.Serial != "" {
		rows = append(rows, cardRow{"Serial", d.Serial})
	}
	rows = append(rows,
		cardRow{"Disk", diskLine(d)},
		cardRow{"Writes", writes + " · the partition is backed up first and read back after"},
	)
	return rows
}

// diskNumRE is the OS's number at the end of a disk path: PhysicalDrive2,
// /dev/disk4.
var diskNumRE = regexp.MustCompile(`(\d+)$`)

// diskLine is "disk 2 of 3 on this PC · 74.5 GB", with the OS's own disk
// number (the one Disk Management shows).
func diskLine(d Device) string {
	size := ""
	if d.Size > 0 {
		v, unit := humanBinary(d.Size)
		size = " · " + v + " " + unit
	}
	m := diskNumRE.FindStringSubmatch(d.Path)
	if m == nil {
		return "this disk" + size
	}
	n, _ := strconv.Atoi(m[1])
	if d.DiskCount > n {
		return fmt.Sprintf("disk %d of %d on this PC%s", n, d.DiskCount, size)
	}
	return fmt.Sprintf("disk %d%s", n, size)
}

// writeNote is the sentence under the card.
const writeNote = "Nothing on the music volume changes. If anything goes wrong, hold Select+Menu, " +
	"then Select+Play at the Apple logo: that is disk mode, it is in ROM, and this window " +
	"will find the iPod again."

// untestedWarn is the line an untested iPod gets above its typed field.
const untestedWarn = "This hardware has not been tested with Core. Type the disk path to continue."

// confirmWrite is the flasher's Confirm for one write. It runs on the job
// goroutine; everything it needs from the model is in g.
func (u *UI) confirmWrite(ctx context.Context, plan *strings.Builder, g writeGate) func(prompt string) (string, error) {
	return func(prompt string) (string, error) {
		want, ok := devicePathFromPrompt(prompt)
		if !ok {
			// The flasher's wording changed under us. Refusing here is
			// the only honest move: a dialog that cannot say what to
			// type would either wave the write through or fail it
			// with a message about a path nobody was shown.
			return "", fmt.Errorf("the flasher's confirmation prompt is not in the form core-app knows: %q", prompt)
		}
		var size int64
		if fi, err := os.Stat(g.image); err == nil {
			size = fi.Size()
		}
		req, err := writeDialog(g, want, size, strings.TrimSpace(tail(plan.String(), 60)))
		if err != nil {
			return "", err
		}
		return u.ask(ctx, req)
	}
}

// writeDialog is the request confirmWrite raises: the identity card, the
// note, the plan behind its link, and the gate — the hold on tested
// hardware (refused outright if the flasher is asking about a path other
// than the one this window identified), the typed path everywhere else.
func writeDialog(g writeGate, want string, size int64, plan string) (*dialogRequest, error) {
	req := &dialogRequest{
		title: g.title,
		card:  identityCard(g.dev, size),
		note:  writeNote,
		plan:  plan,
	}
	if runtime.GOOS == "windows" && !g.dev.Elevated && g.cli != "" {
		req.note += " Windows will ask for permission to run " + fileBase(g.cli) + ", which does the write."
	}
	if g.dev.Tested {
		if want != g.dev.Path {
			return nil, fmt.Errorf("the flasher asked to write %s, but this window identified the iPod as %s — refusing",
				want, g.dev.Path)
		}
		req.hold, req.holdAnswer = true, g.dev.Path
		req.okLabel = "Hold to write…"
		return req, nil
	}
	req.warn = untestedWarn
	req.prompt = "Type the disk path  " + want + "  to continue:"
	req.want = want
	req.okLabel = "Write it"
	return req, nil
}

// holdTick advances the hold-to-write button by one frame. pressed is
// whether the pointer is down on it now; it returns how far the fill has
// grown (0..1) and whether this frame completed the hold. Releasing early
// starts again from nothing.
func (u *UI) holdTick(pressed bool, now time.Time) (float32, bool) {
	if !pressed {
		u.holdStart = time.Time{}
		return 0, false
	}
	if u.holdStart.IsZero() {
		u.holdStart = now
	}
	f := float32(now.Sub(u.holdStart)) / float32(HoldDuration)
	if f >= 1 {
		u.holdStart = time.Time{}
		return 1, true
	}
	return f, false
}

// removalDialog is Sync and remove's second step: the albums that would be
// deleted, their sizes, the total, and a button that says how many.
// size reads one file's size (statSize; a test passes its own).
func removalDialog(plan *syncer.Plan, size func(string) int64) *dialogRequest {
	type album struct {
		name  string
		bytes int64
	}
	groups := map[string]*album{}
	var order []string
	for _, p := range plan.Prune {
		name := albumOf(p)
		a := groups[name]
		if a == nil {
			a = &album{name: name}
			groups[name] = a
			order = append(order, name)
		}
		a.bytes += size(p)
	}
	sort.Strings(order)
	const maxRows = 12
	var rows []cardRow
	for i, name := range order {
		if i == maxRows {
			rows = append(rows, cardRow{fmt.Sprintf("… and %d more", len(order)-maxRows), ""})
			break
		}
		v := ""
		if b := groups[name].bytes; b > 0 {
			n, unit := humanBinary(b)
			v = n + " " + unit
		}
		rows = append(rows, cardRow{name, v})
	}
	n := len(order)
	total := fmt.Sprintf("%d album%s", n, plural(n))
	if plan.PruneBytes > 0 {
		v, unit := humanBinary(plan.PruneBytes)
		total += " · " + v + " " + unit
	}
	return &dialogRequest{
		title:   "Delete these from the iPod?",
		items:   rows,
		total:   total + " will be deleted from the iPod",
		note:    "These are on the iPod but not in your music folder. The sync runs right after.",
		okLabel: fmt.Sprintf("Delete %d album%s and sync", n, plural(n)),
	}
}

// albumOf is the album folder a pruned path belongs to: the component after
// Music, or "Loose files" for something directly in Music.
func albumOf(p string) string {
	parts := strings.FieldsFunc(filepath.ToSlash(p), func(r rune) bool { return r == '/' || r == '\\' })
	for i, s := range parts {
		if strings.EqualFold(s, "Music") && i+1 < len(parts) {
			if i+2 < len(parts) || !looseExt[strings.ToLower(filepath.Ext(parts[i+1]))] {
				return parts[i+1]
			}
			return "Loose files"
		}
	}
	if len(parts) >= 2 {
		return parts[len(parts)-2]
	}
	return "Loose files"
}

// looseExt are the file types that can sit directly in Music/ — anything
// else there is an album folder.
var looseExt = map[string]bool{".flac": true, ".mp3": true, ".m4a": true, ".art": true,
	".thm": true, ".jpg": true, ".png": true, ".m3u": true, ".m3u8": true, ".txt": true}

// statSize is a file's size, or 0.
func statSize(p string) int64 {
	if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
		return fi.Size()
	}
	return 0
}
