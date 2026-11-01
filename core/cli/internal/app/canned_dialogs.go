package app

import (
	"fmt"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// DialogKinds are the dialogs `core-app --screenshot --dialog KIND` can
// put on screen, for review. They are drawn and nothing else: a canned
// dialog has no job behind it, so nothing can be confirmed through one.
var DialogKinds = []string{"write", "holding", "untested", "remove", "about"}

// cannedDialog builds one of DialogKinds against the state's device.
func cannedDialog(u *UI, kind string) error {
	g := u.gate("Update this iPod to Core " + u.st.Release.Tag + "?")
	switch kind {
	case "write", "holding":
		req, err := writeDialog(g, g.dev.Path, 443_392, strings.TrimSpace(flashPlanSample))
		if err != nil {
			return err
		}
		u.dlg = req
		if kind == "holding" {
			u.holdFrozen = 0.45
		}
	case "untested":
		g.dev.Tested = false
		req, err := writeDialog(g, g.dev.Path, 443_392, strings.TrimSpace(flashPlanSample))
		if err != nil {
			return err
		}
		u.dlg = req
		u.confirmEd.SetText(`\\.\PhysicalDrive`)
	case "remove":
		plan := &syncer.Plan{PruneBytes: 1_288_490_189, Prune: []string{
			`D:\Music\Old Band - First Album\01. Intro.flac`,
			`D:\Music\Old Band - First Album\folder.art`,
			`D:\Music\Someone Else - Live at the Hall\01. Opening.flac`,
			`D:\Music\Someone Else - Live at the Hall\02. Encore.flac`,
			`D:\Music\The Leftovers - Demos\01. Demo.flac`,
		}}
		sizes := map[string]int64{
			plan.Prune[0]: 412_000_000, plan.Prune[2]: 330_000_000,
			plan.Prune[3]: 320_000_000, plan.Prune[4]: 226_490_189,
		}
		u.dlg = removalDialog(plan, func(p string) int64 { return sizes[p] })
	case "about":
		u.showAbout()
		return nil
	default:
		return fmt.Errorf("--dialog %q: want one of %s", kind, strings.Join(DialogKinds, ", "))
	}
	u.dlg.reply = make(chan string, 1)
	return nil
}

// flashPlanSample is what the flasher prints before it asks, for the canned
// write dialogs' "Show the plan".
var flashPlanSample = `
device      \\.\PhysicalDrive2  iPod Video 5.5G 80 GB  80.0 GB
partition   0: 129,024 .. 131,604,480 (131,475,456 bytes), sector 2048
image       C:\Users\brandon-home\AppData\Roaming\core\cache\v0.1.3\core.ipod
            443,392 bytes, checksum 0x0163b2a0 (.ipod header verified)

OSOS entry  devOffset 0x4800, body at 0x5000, capacity 7,618,560
  old       len 237,640  chksum 0x01589d64
  new       len 443,392  chksum 0x0163b2a0

backup      C:\Users\brandon-home\AppData\Roaming\core\backups\fwpart-131475456-2026-09-15T01-12-04Z.bin
` + "\n" + flasher.ConfirmPrompt(`\\.\PhysicalDrive2`)
