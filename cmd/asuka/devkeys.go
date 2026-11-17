package main

import "os"

// devKeys reads the keys pressed in asuka dev's terminal, one at a time,
// as they're pressed (not a line's worth on Enter), without echoing
// them; Ctrl-C still stops asuka dev, and what it prints still breaks
// lines as it did. restore puts the terminal back as it was. Not a
// terminal (asuka dev's output piped, its tests): no keys.
func devKeys() (keys <-chan byte, restore func()) {
	restore, ok := rawKeys(os.Stdin)
	if !ok {
		return nil, func() {}
	}
	c := make(chan byte, 8)
	go func() {
		b := make([]byte, 16)
		for {
			n, err := os.Stdin.Read(b)
			for _, k := range b[:n] {
				select {
				case c <- k:
				default: // keys pressed faster than the loop takes them
				}
			}
			if err != nil {
				return
			}
		}
	}()
	return c, restore
}
