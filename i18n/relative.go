package i18n

import (
	"strings"
	"time"
)

// RelativeStyle is how long FormatRelative's words are, CLDR's widths of
// relative times: "in 3 hours", "in 3 hr.", "in 3h" in English.
type RelativeStyle uint8

const (
	RelativeLong RelativeStyle = iota
	RelativeShort
	RelativeNarrow
)

// The units of relative times, in the order of the slots
// (i18n/internal/gen's relativeUnits).
const (
	unitSecond = iota
	unitMinute
	unitHour
	unitDay
	unitWeek
	unitMonth
	unitYear
)

// relativeParts is how many slots a unit has in a width: its future
// patterns, its past ones and its named forms (i18n/internal/gen).
const relativeParts = 3

// RelativeUnit picks the unit FormatRelative says t in, seen from now,
// and how many of it (negative in the past), as Foundation's
// RelativeDateTimeFormatter does: the calendar's difference between the
// two in now's location, in years, months, weeks, days, hours, minutes
// and seconds, and the largest of those that isn't 0, its count
// truncated ("1 day ago" until it's 2 days; "23 hours ago" until a day,
// a calendar one: yesterday at 23:00 is "2 hours ago" at 01:00). Under a
// second is 0 seconds.
func RelativeUnit(t, now time.Time) (unit int, n int64) {
	t = t.In(now.Location())
	a, b, sign := now, t, int64(1)
	if t.Before(now) {
		a, b, sign = t, now, -1
	}
	// Whole months: b's place in its month before a's in its own takes
	// one off.
	months := int64(b.Year()-a.Year())*12 + int64(b.Month()-a.Month())
	if months > 0 && inMonth(b) < inMonth(a) {
		months--
	}
	switch {
	case months >= 12:
		return unitYear, sign * (months / 12)
	case months > 0:
		return unitMonth, sign * months
	}
	days := civilDays(b) - civilDays(a)
	if days > 0 && inDay(b) < inDay(a) {
		days--
	}
	switch {
	case days >= 7:
		return unitWeek, sign * (days / 7)
	case days > 0:
		return unitDay, sign * days
	}
	d := b.Sub(a)
	switch {
	case d >= time.Hour:
		return unitHour, sign * int64(d/time.Hour)
	case d >= time.Minute:
		return unitMinute, sign * int64(d/time.Minute)
	}
	return unitSecond, sign * int64(d/time.Second)
}

// inMonth is t's place in its month, to compare: its day, then its time.
func inMonth(t time.Time) int64 { return int64(t.Day())*86400e9 + inDay(t) }

// inDay is t's time of day, in nanoseconds.
func inDay(t time.Time) int64 {
	return int64(t.Hour())*3600e9 + int64(t.Minute())*60e9 + int64(t.Second())*1e9 + int64(t.Nanosecond())
}

// civilDays is the number of t's date counted from 1970-01-01.
func civilDays(t time.Time) int64 {
	return time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, time.UTC).Unix() / 86400
}

// FormatRelative writes t, seen from now, in the words of locale: "in 3
// hours", "2 days ago", in the unit RelativeUnit picks. Unless numeric,
// an offset the locale names is said by its name: "now" (under a
// second), "yesterday", "tomorrow", "last week", "next year".
func FormatRelative(locale string, t, now time.Time, style RelativeStyle, numeric bool) string {
	unit, n := RelativeUnit(t, now)
	if style > RelativeNarrow {
		style = RelativeLong
	}
	d := keyOf(locale).dates()
	at := slotRelative + (unit*3+int(style))*relativeParts
	if !numeric && n >= -2 && n <= 2 && (n != 0 || unit == unitSecond) {
		if s := strings.Split(d.v[at+2], "\x1f"); len(s) == 5 && s[n+2] != "" {
			return s[n+2]
		}
	}
	part, abs := d.v[at], n
	if n < 0 {
		part, abs = d.v[at+1], -n
	}
	pats := strings.Split(part, "\x1f")
	if len(pats) != 6 {
		return FormatNumber(locale, n, NumberOptions{})
	}
	p := pats[PluralCategory(locale, intOperands(abs))]
	if p == "" {
		p = pats[PluralOther]
	}
	return strings.Replace(p, "{0}", FormatNumber(locale, abs, NumberOptions{Integer: true}), 1)
}
