"""Which titles are solo piano recordings: the rules the YouTube window dims and
counts by, and that a playlist or channel run filters by.

The window reads the same rules from rules_json(), served as
/quartzmidi/rules.json, and builds each regex from its source and flags:

- Text is NFKC-normalised, then lowercased.
- A title is other when it has a LESSON term that the query lacks.
- Otherwise a SOLO term anywhere in a title means solo piano, whatever else it names.
- Otherwise a title is other when it has an OTHER term that the query lacks,
  unless the title also has one of that term's UNLESS terms.
- Latin terms match between non-letters, CJK terms as substrings. A space in a
  term matches any run of spaces, or none at all in a CJK term.
"""

import json
import re
import sys
import unicodedata

SOLO = ("solo piano", "piano solo", "ピアノソロ", "피아노 솔로", "钢琴独奏", "鋼琴獨奏", "ピアノ独奏")

# Another instrument, a voice, a group, an arrangement or a reaction. Band, bass,
# duet and lyrics are common in song names, so only their phrases count.
OTHER = (
    "orchestra", "orchestral", "symphony", "ensemble", "quartet", "trio", "4 hands", "4-hands", "four hands",
    "two pianos", "2 pianos", "piano duo", "piano duet", "duet ver", "violin", "viola", "cello", "flute",
    "clarinet", "sax", "saxophone", "trumpet", "harp", "guitar", "bass guitar", "bass cover", "drum", "drums",
    "full band", "band cover", "organ", "strings", "accordion", "ukulele", "kalimba", "marimba", "music box",
    "voice", "vocal", "vocals", "singing", "sings", "sing along", "choir", "karaoke", "acapella", "a cappella",
    "lyric video", "lyrics video", "with lyrics", "lofi", "lo-fi", "remix", "react", "reacts", "reaction", "concerto",
    "concerto da camera", "連弾", "四手", "연탄", "合唱", "歌ってみた", "弾き語り", "歌詞", "バイオリン", "ヴァイオリン", "바이올린", "小提琴",
    "チェロ", "大提琴", "ギター", "吉他", "オーケストラ", "管弦楽", "交响", "오케스트라", "二胡", "古箏",
)

# A title takes back these OTHER terms with one of their UNLESS terms. A concerto
# can be solo by nature: Bach's Italian Concerto and his keyboard transcriptions
# (BWV 971-987), and Alkan's Concerto for solo piano; his Concerti da camera have
# strings, and "concerto da camera" stays other. Or the title marks a solo
# arrangement. "Minus orchestra" is a concerto's solo part alone.
NO_ORCHESTRA = ("without orchestra", "minus orchestra", "no orchestra", "sans orchestre")
UNLESS = {
    "concerto": (
        "italian concerto", "concerto italien", "concerto italiano", "italian style", "italienischem gusto", "alkan",
        *(f"bwv{gap}{number}" for number in range(971, 988) for gap in (" ", "")),
        "solo", "piano seul", "piano alone", *NO_ORCHESTRA, "piano cover", "piano version", "piano arrangement",
        "arranged for piano", "arr. for piano", "piano transcription", "transcribed for piano", "stride",
    ),
    "orchestra": NO_ORCHESTRA,
}

# Teaching, quizzes and talk rather than a performance; a SOLO term does not save
# them ("Piano Solo Tutorial"). Synthesia, sheet music, medley, learn, vs,
# challenge, chords, left or right hand, step by step and a bare "beginner",
# "how to" or "101" also name performances ("How to Train Your Dragon", "101
# Dalmatians"), so they are left out.
LESSON = (
    "tutorial", "how to play", "lesson", "lessons", "for beginners", "beginner piano", "easy piano", "piano easy",
    "easy version", "easy ver", "slow version", "learn to play", "play along", "quiz", "interview", "evolution of",
    "easy to impossible", "levels of", "technique", "techniques", "masterclass", "master class", "crash course",
    "explained", "piano 101",
    "弾き方", "初心者", "簡単", "講座", "강좌", "레슨", "쉬운", "教学", "教程",
)

# A query with one of these needs no " piano" added. Substrings, so "pianos" and
# "pianista" count too.
PIANO_WORDS = ("piano", "pianist", "ピアノ", "피아노", "钢琴", "鋼琴")

# Hangul, kana and Han: scripts written without spaces between words.
CJK = re.compile("[\u1100-\u11ff\u3040-\u30ff\u3130-\u318f\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uf900-\ufaff]")
SYNTAX = set("^$\\.*+?()[]{}|/")
JS_LETTER = r"[\p{L}\p{N}]"  # with the "u" flag
PY_LETTER = r"[^\W_]"  # the same set: Python's re has no \p{}


def source(term, letter=JS_LETTER):
    """The regex source for one term, in JavaScript's syntax unless letter is PY_LETTER."""
    cjk = bool(CJK.search(term))
    body = "".join((r"\s*" if cjk else r"\s+") if c == " " else "\\" + c if c in SYNTAX else c for c in term)
    return body if cjk else f"(?<!{letter}){body}(?!{letter})"


def normalize(text):
    """NFKC, then lowercase: the form every rule is matched against."""
    return unicodedata.normalize("NFKC", str(text or "")).lower()


SOLO_RULES = tuple(re.compile(source(term, PY_LETTER)) for term in SOLO)
OTHER_RULES = tuple(re.compile(source(term, PY_LETTER)) for term in OTHER)
LESSON_RULES = tuple(re.compile(source(term, PY_LETTER)) for term in LESSON)
# Each OTHER rule's UNLESS rules, in OTHER's order; empty for most.
UNLESS_RULES = tuple(tuple(re.compile(source(word, PY_LETTER)) for word in UNLESS.get(term, ())) for term in OTHER)


def is_other(title, query=""):
    """True when the title names something besides a solo piano performance that the query does not ask for."""
    title, query = normalize(title), normalize(query)
    if any(rule.search(title) and not rule.search(query) for rule in LESSON_RULES):
        return True
    if any(rule.search(title) for rule in SOLO_RULES):
        return False
    return any(rule.search(title) and not rule.search(query) and not any(back.search(title) for back in unless)
               for rule, unless in zip(OTHER_RULES, UNLESS_RULES))


def has_piano(query):
    """True when the query already asks for piano."""
    query = normalize(query)
    return any(word in query for word in PIANO_WORDS)


def rules_json():
    """The rules as the window reads them: each term with its regex source and flags, and an OTHER term's
    UNLESS terms as its "unless" list."""
    def terms(words, unless=None):
        entries = [{"term": word, "source": source(word), "flags": "u"} for word in words]
        for entry in entries:
            if unless and entry["term"] in unless:
                entry["unless"] = terms(unless[entry["term"]])
        return entries

    return json.dumps({"normalize": "NFKC", "lowercase": True, "solo": terms(SOLO), "other": terms(OTHER, UNLESS),
                       "lesson": terms(LESSON), "piano": list(PIANO_WORDS)}, ensure_ascii=False, indent=1) + "\n"


if __name__ == "__main__":
    # tests/fixtures/rules.json is this output.
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    sys.stdout.write(rules_json())
