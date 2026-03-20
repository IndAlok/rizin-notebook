package main

import (
	"crypto/rand"
	"strings"
)

// ElementNonceSize is the length of nonce strings used for page elements.
const ElementNonceSize = 8

// PageNonceSize is the length of nonce strings used for notebook pages.
const PageNonceSize = 32

// nonceAlphabet is the character set used for generating nonce strings.
const nonceAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

func Nonce(n int) string {
	const modulo = byte(len(nonceAlphabet))
	bytes := make([]byte, n)
	if _, err := rand.Read(bytes); err != nil {
		panic(err)
	}
	for i, b := range bytes {
		bytes[i] = nonceAlphabet[b%modulo]
	}
	return string(bytes)
}

func IsValidNonce(nonce string, expectedLen int) bool {
	if len(nonce) != expectedLen {
		return false
	}
	for _, c := range nonce {
		if !strings.ContainsRune(nonceAlphabet, c) {
			return false
		}
	}
	return true
}
