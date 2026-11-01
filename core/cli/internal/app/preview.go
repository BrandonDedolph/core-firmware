package app

import "fmt"

// The sync preview: what a sync will copy, shown before it copies it. The
// header's "Sync N albums" and the iPod tab's "Preview sync" both open it
// (startSyncPreview); its button runs the sync. Nothing on the iPod changes
// until that button is pressed.

// previewMax is how many albums the preview lists before "… and N more".
const previewMax = 40

// previewDialog is the preview for the albums one plan would copy. src names
// the music folder in the total line; tooOld turns the button off for an
// iPod whose firmware cannot read what a sync writes.
func previewDialog(albums []Album, src string, tooOld bool) *dialogRequest {
	var rows []cardRow
	n, tracks := 0, 0
	var bytes int64
	for _, a := range albums {
		if a.Copy == 0 {
			continue
		}
		n++
		tracks += a.Copy
		bytes += a.Bytes
		if n > previewMax {
			continue
		}
		mid := fmt.Sprintf("%d of %d tracks", a.Copy, a.Tracks)
		if a.Copy == a.Tracks {
			mid = fmt.Sprintf("all %d track%s", a.Tracks, plural(a.Tracks))
		}
		size := ""
		if a.Bytes > 0 {
			v, unit := humanBinary(a.Bytes)
			size = v + " " + unit
		}
		rows = append(rows, cardRow{label: a.Title, mid: mid, value: size})
	}
	if n == 0 {
		return &dialogRequest{
			title:   "Everything is on the iPod",
			note:    "Your music folder (" + sourceName(src) + ") and the iPod match: a sync would copy nothing.",
			okLabel: "Close",
			only:    true,
		}
	}
	if n > previewMax {
		rows = append(rows, cardRow{label: fmt.Sprintf("… and %d more", n-previewMax)})
	}
	total := fmt.Sprintf("%d track%s", tracks, plural(tracks))
	if bytes > 0 {
		v, unit := humanBinary(bytes)
		total += " · " + v + " " + unit
	}
	total += " · from " + sourceName(src)
	req := &dialogRequest{
		title:   fmt.Sprintf("Preview: %d album%s to copy", n, plural(n)),
		items:   rows,
		total:   total,
		note:    "Nothing has been written. Sync copies these to Music/ on the iPod and rebuilds the index.",
		okLabel: fmt.Sprintf("Sync %d album%s", n, plural(n)),
	}
	if tooOld {
		req.note = "Nothing has been written. " + upperFirst(TooOldText) + "."
		req.okLabel, req.only = "Close", true
	}
	return req
}

func upperFirst(s string) string {
	if s == "" {
		return s
	}
	if c := s[0]; c >= 'a' && c <= 'z' {
		return string(c-'a'+'A') + s[1:]
	}
	return s
}
