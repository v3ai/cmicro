// Reference side of the regex differential test (see run.sh): reads the
// same "FLAGS xHEXPATTERN xHEXSUBJECT" lines as tests/regex_diff.c and prints
// Go regexp's FindAllSubmatchIndex result in the same format.
package main

import (
	"bufio"
	"encoding/hex"
	"os"
	"regexp"
	"strconv"
	"strings"
)

func main() {
	in := bufio.NewReaderSize(os.Stdin, 1<<22)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for {
		line, err := in.ReadString('\n')
		if len(line) == 0 && err != nil {
			break
		}
		f := strings.Fields(line)
		if len(f) < 2 {
			continue
		}
		pb, _ := hex.DecodeString(f[1][1:])
		var sb []byte
		if len(f) > 2 {
			sb, _ = hex.DecodeString(f[2][1:])
		}
		pat := string(pb)
		if strings.Contains(f[0], "l") {
			pat = regexp.QuoteMeta(pat)
		}
		if strings.Contains(f[0], "i") {
			pat = "(?i)" + pat
		}
		re, e := regexp.Compile(pat)
		if e != nil {
			out.WriteString("E\n")
			continue
		}
		for k, m := range re.FindAllSubmatchIndex(sb, -1) {
			if k > 0 {
				out.WriteByte(';')
			}
			for i, v := range m {
				if i > 0 {
					out.WriteByte(' ')
				}
				out.WriteString(strconv.Itoa(v))
			}
		}
		out.WriteByte('\n')
	}
}
