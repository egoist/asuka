//go:build !unix

package main

import "time"

// lchtimes dates the link p itself: a Mac app's links are dated on a
// Mac, where its archives are made.
func lchtimes(p string, t time.Time) error { return nil }
