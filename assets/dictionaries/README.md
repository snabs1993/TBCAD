# Dictionaries

`thai-words.txt` is the Thai word list used by `tbcad::text` for MTEXT line breaking
(`libs/text/src/thai_break.cpp`). One word per line, UTF-8, sorted, 60,958 words.

* Source: PyThaiNLP `pythainlp/corpus/words_th.txt` (https://github.com/PyThaiNLP/pythainlp),
  released under CC0 1.0 Universal (public domain dedication), see
  `third_party/notices/thai-wordlist.txt`.
* Filtering done for TBCAD: kept only entries made of Thai letters and marks (U+0E01..U+0E4E) with at
  least two characters that do not start with a vowel sign, tone mark or repetition sign; removed
  duplicates; sorted.
* Not derived from libthai or ICU dictionaries (those carry other licences).

To replace or extend the list edit this file; no code change is needed. One word per line.
