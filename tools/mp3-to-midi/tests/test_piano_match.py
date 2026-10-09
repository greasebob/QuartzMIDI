"""piano_match: the shared title rules, and rules.json as the window reads it."""

import json
import os
import re
import sys
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(TESTS, "fixtures")
# The embedded Python's ._pth file leaves the converter's folder off the path.
sys.path.insert(0, os.path.dirname(TESTS))

import piano_match
from piano_match import has_piano, is_other


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as file:
        return json.load(file)


class SharedTitles(unittest.TestCase):
    def test_titles_fixture(self):
        cases = fixture("titles.json")
        self.assertGreater(len(cases), 50)
        for case in cases:
            with self.subTest(title=case["title"], query=case["query"]):
                self.assertIs(is_other(case["title"], case["query"]), case["expected"])

    def test_fixture_covers_every_term(self):
        # Each OTHER and LESSON term is hit by some title, so a broken source shows
        # up here and in the window's rules test alike.
        titles = [piano_match.normalize(case["title"]) for case in fixture("titles.json") if case["expected"]]
        terms = piano_match.OTHER + piano_match.LESSON
        for term, rule in zip(terms, piano_match.OTHER_RULES + piano_match.LESSON_RULES):
            if term in ("orchestra", "symphony", "ensemble", "viola", "flute", "clarinet", "sax", "trumpet", "harp",
                        "guitar", "drum", "vocal", "singing", "sings", "sing along", "acapella", "react", "연탄",
                        "바이올린", "小提琴"):
                continue  # covered by the unit tests below
            with self.subTest(term=term):
                self.assertTrue(any(rule.search(title) for title in titles))

    def test_recordings_fixture(self):
        # 80 titles from saved signed-out searches, the first five new videos of each of 16 pages, labelled by
        # hand. On these plain rows the page's isRecording is the title rules, 1 to 20 minutes and chapters naming
        # at most one piece; the fixture keeps only a count of chapters, and the page takes each untitled one as
        # its own piece. The misses are a trio, a live ensemble and a compilation the titles do not name, and solo
        # recordings with chapters or "with lyrics"; there must be no more of them (the same limits as rules_test.js).
        cases = fixture("recordings.json")
        self.assertEqual(len(cases), 80)
        counted, missed = [], []
        for case in cases:
            rules = (not is_other(case["title"], case["query"]) and 60 <= case["seconds"] <= 1200
                     and case["chapters"] < 2)
            if rules and not case["recording"]:
                counted.append(case["title"])
            if case["recording"] and not rules:
                missed.append(case["title"])
        self.assertLessEqual(len(counted), 3, counted)
        self.assertLessEqual(len(missed), 8, missed)


class Rules(unittest.TestCase):
    def test_latin_terms_need_non_letters_around_them(self):
        self.assertTrue(is_other("Violin Sonata"))
        self.assertTrue(is_other("violin."))
        self.assertTrue(is_other("(violin)"))
        self.assertTrue(is_other("piano_violin"))  # "_" is not a letter
        self.assertFalse(is_other("Violinist plays Chopin"))
        self.assertFalse(is_other("Harpsichord Suite"))
        self.assertFalse(is_other("Saxony"))
        self.assertTrue(is_other("Flute 2"))
        self.assertFalse(is_other("Flute2"))  # a digit counts as a letter here
        self.assertTrue(is_other("Ｓａｘ"))  # NFKC folds full-width letters

    def test_cjk_terms_match_as_substrings(self):
        self.assertTrue(is_other("千本桜バイオリンとピアノ"))
        self.assertTrue(is_other("봄날바이올린"))
        self.assertTrue(is_other("梁祝小提琴协奏曲"))
        self.assertTrue(is_other("ピアノ連弾"))
        self.assertTrue(is_other("피아노연탄"))
        self.assertTrue(is_other("ﾊﾞｲｵﾘﾝ"))  # half-width kana, folded by NFKC

    def test_solo_wins_outright(self):
        for title in ("Piano Solo with Orchestra", "solo piano - violin cover", "ピアノソロ 連弾",
                      "피아노 솔로 바이올린", "피아노솔로 바이올린", "钢琴独奏 小提琴", "鋼琴獨奏 大提琴", "ピアノ独奏 合唱"):
            with self.subTest(title=title):
                self.assertFalse(is_other(title))
        self.assertTrue(is_other("Solo Pianist with Orchestra"))

    def test_query_exemption(self):
        self.assertTrue(is_other("Canon - Violin & Piano"))
        self.assertFalse(is_other("Canon - Violin & Piano", "canon violin"))
        self.assertFalse(is_other("Canon - Violin & Piano", "ＶＩＯＬＩＮ"))
        self.assertTrue(is_other("Canon - Violin & Cello", "violin"))
        self.assertTrue(is_other("Canon - Violin & Piano", "violinist"))
        self.assertFalse(is_other("千本桜 連弾", "連弾 千本桜"))
        self.assertFalse(is_other("Drums and Piano", "drums"))
        self.assertTrue(is_other("Drums and Piano", "drum"))  # each term on its own
        self.assertTrue(is_other("Piano Duet", "duet"))
        self.assertFalse(is_other("Piano Duet", "piano duet"))

    def test_concerto_unless_solo(self):
        self.assertIn("concerto", piano_match.OTHER)
        self.assertTrue(is_other("Rachmaninoff - Piano Concerto No. 2"))
        self.assertFalse(is_other("Concerto No. 2 for solo piano"))
        self.assertFalse(is_other("Rachmaninoff - Piano Concerto No. 2", "rachmaninoff concerto"))
        # Solo by nature, or a solo arrangement; an orchestra or another instrument named as well still counts.
        for title in ("Italian Concerto", "Bach - Concerto in D minor, BWV 974", "Bach - Concerto, BWV987",
                      "Alkan - Concerto, Op. 39", "Concerto pour piano seul", "Concerto (Solo Version)",
                      "Concerto #2 minus orchestra", "Concerto without Orchestra", "Concerto (Piano Arrangement)",
                      "Concerto arr. for piano", "Concerto 2 (Stride Piano)"):
            with self.subTest(title=title):
                self.assertFalse(is_other(title))
        for title in ("Bach - Concerto in A minor, BWV 1065", "Bach - Concerto, BWV 1052", "Bach - Concerto, BWV 9710",
                      "Alkan - Concerto da camera No. 2", "Concerto (Solo) with Orchestra", "Italian Concerto - Violin",
                      "Italian Concerto - Piano Tutorial", "Orchestra minus piano"):
            with self.subTest(title=title):
                self.assertTrue(is_other(title))

    def test_unless_terms_take_back_their_term_only(self):
        self.assertLessEqual(set(piano_match.UNLESS), set(piano_match.OTHER))
        for term, backs in piano_match.UNLESS.items():
            for back in backs:
                with self.subTest(term=term, back=back):
                    self.assertTrue(is_other(f"Song {term}"))
                    self.assertFalse(is_other(f"Song {term} {back}"))
                    self.assertTrue(is_other(f"Song {term} {back} cello"))
                    self.assertTrue(is_other(f"Song {term} {back} tutorial"))

    def test_lesson_terms(self):
        for term in piano_match.LESSON:
            with self.subTest(term=term):
                self.assertTrue(is_other(f"Song - {term}"))
                self.assertFalse(is_other(f"Song - {term}", f"song {term}"))

    def test_solo_does_not_save_a_lesson(self):
        # SOLO overrides OTHER only.
        for title in ("Nocturne - Piano Solo Tutorial", "solo piano lesson", "ピアノソロ 弾き方", "피아노 솔로 강좌",
                      "钢琴独奏 教学"):
            with self.subTest(title=title):
                self.assertTrue(is_other(title))
        self.assertFalse(is_other("Piano Solo Tutorial with Violin", "tutorial"))
        self.assertTrue(is_other("Piano Tutorial with Violin", "tutorial"))

    def test_lesson_query_exemption(self):
        self.assertFalse(is_other("Für Elise - Piano Tutorial", "für elise tutorial"))
        self.assertFalse(is_other("Für Elise - Piano Tutorial", "ＴＵＴＯＲＩＡＬ"))
        self.assertTrue(is_other("Für Elise - Piano Tutorial", "tutorials"))  # each term on its own
        self.assertTrue(is_other("How to Play Für Elise", "how to"))
        self.assertFalse(is_other("ピアノ講座 第1回", "講座"))
        self.assertTrue(is_other("Easy Piano Tutorial", "easy piano"))  # the query does not ask for "tutorial"

    def test_left_out(self):
        # These name performances too, so they never dim a title.
        for word in ("synthesia", "sheet music", "medley", "learn", "vs", "challenge", "chords", "left hand",
                     "right hand", "step by step", "beginner", "how to", "101"):
            with self.subTest(word=word):
                self.assertNotIn(word, piano_match.LESSON + piano_match.OTHER)
                self.assertFalse(is_other(f"{word} - Song"))

    def test_band_bass_duet_and_lyrics_count_only_as_phrases(self):
        for title in ("Band of Brothers", "All About That Bass", "Duet", "Lyrics", "The Band - Piano",
                      "Bass Drop Piano", "Lyric Piano"):
            with self.subTest(title=title):
                self.assertFalse(is_other(title))
        for title in ("full band", "band cover", "bass cover", "bass guitar", "piano duet", "duet ver.",
                      "lyric video", "with lyrics", "lyrics video"):
            with self.subTest(title=title):
                self.assertTrue(is_other(f"Song - {title}"))

    def test_the_added_terms(self):
        for term in ("voice", "strings", "organ", "lofi", "lo-fi", "remix", "4-hands", "four hands", "2 pianos",
                     "piano duo", "trio", "accordion", "ukulele", "kalimba", "marimba", "music box", "連弾", "四手",
                     "연탄", "合唱", "歌ってみた", "弾き語り", "歌詞", "バイオリン", "ヴァイオリン", "바이올린", "小提琴",
                     "チェロ", "大提琴", "ギター", "吉他", "オーケストラ", "管弦楽", "交响", "오케스트라", "二胡", "古箏"):
            with self.subTest(term=term):
                self.assertIn(term, piano_match.OTHER)
                self.assertTrue(is_other(f"Song {term} cover"))

    def test_the_current_terms_stay(self):
        for term in ("orchestra", "orchestral", "symphony", "ensemble", "quartet", "4 hands", "four hands",
                     "two pianos", "violin", "viola", "cello", "flute", "clarinet", "sax", "saxophone", "trumpet",
                     "harp", "guitar", "drum", "drums", "vocal", "vocals", "singing", "sings", "sing along", "choir",
                     "karaoke", "acapella", "a cappella", "react", "reacts", "reaction"):
            with self.subTest(term=term):
                self.assertTrue(is_other(f"Song {term}"))

    def test_spaces_in_terms(self):
        self.assertTrue(is_other("Piano  Four   Hands"))
        self.assertFalse(is_other("Nocturne (Piano\u3000Solo) Violin"))  # NFKC turns U+3000 into a space
        self.assertFalse(is_other("피아노솔로 바이올린"))

    def test_odd_input(self):
        self.assertFalse(is_other(""))
        self.assertFalse(is_other(None))
        self.assertTrue(is_other("Violin", None))


class HasPiano(unittest.TestCase):
    def test_has_piano(self):
        for query in ("solo piano", "PIANO", "ＰＩＡＮＯ", "pianist", "Pianos", "pianista", "千本桜 ピアノ", "ﾋﾟｱﾉ",
                      "봄날 피아노", "卡农 钢琴", "卡農 鋼琴"):
            with self.subTest(query=query):
                self.assertTrue(has_piano(query))
        for query in ("", None, "chopin nocturne", "pian", "keyboard"):
            with self.subTest(query=query):
                self.assertFalse(has_piano(query))


class RulesJson(unittest.TestCase):
    def test_equals_the_fixture(self):
        # Regenerate with: python piano_match.py > tests/fixtures/rules.json
        self.assertEqual(json.loads(piano_match.rules_json()), fixture("rules.json"))

    def test_shape(self):
        rules = json.loads(piano_match.rules_json())
        self.assertEqual(rules["normalize"], "NFKC")
        self.assertIs(rules["lowercase"], True)
        self.assertEqual([entry["term"] for entry in rules["solo"]], list(piano_match.SOLO))
        self.assertEqual([entry["term"] for entry in rules["other"]], list(piano_match.OTHER))
        self.assertEqual([entry["term"] for entry in rules["lesson"]], list(piano_match.LESSON))
        self.assertEqual(rules["piano"], list(piano_match.PIANO_WORDS))
        unless = [back for entry in rules["other"] for back in entry.get("unless", [])]
        for entry in rules["solo"] + rules["other"] + rules["lesson"] + unless:
            self.assertEqual(entry["flags"], "u")  # never g or y: test() must not keep state
        for entry in rules["solo"] + rules["lesson"] + unless:
            self.assertNotIn("unless", entry)  # only an OTHER term has exceptions
        self.assertEqual({entry["term"]: tuple(back["term"] for back in entry["unless"])
                          for entry in rules["other"] if "unless" in entry}, piano_match.UNLESS)

    def test_terms_are_already_normalised_and_unique(self):
        for words in (piano_match.SOLO, piano_match.OTHER, piano_match.LESSON, piano_match.PIANO_WORDS,
                      *piano_match.UNLESS.values()):
            self.assertEqual(len(set(words)), len(words))
            for word in words:
                self.assertEqual(piano_match.normalize(word), word)

    def test_python_rules_are_the_served_sources(self):
        # Python's re has no \p{}; [^\W_] is the same set of letters and digits.
        rules = json.loads(piano_match.rules_json())
        served = rules["solo"] + rules["other"] + rules["lesson"]
        served += [back for entry in rules["other"] for back in entry.get("unless", [])]
        compiled = piano_match.SOLO_RULES + piano_match.OTHER_RULES + piano_match.LESSON_RULES
        compiled += tuple(back for backs in piano_match.UNLESS_RULES for back in backs)
        self.assertEqual(len(served), len(compiled))
        for entry, rule in zip(served, compiled):
            with self.subTest(term=entry["term"]):
                self.assertEqual(entry["source"].replace(piano_match.JS_LETTER, piano_match.PY_LETTER), rule.pattern)

    def test_latin_and_cjk_sources(self):
        self.assertEqual(piano_match.source("four hands"),
                         r"(?<![\p{L}\p{N}])four\s+hands(?![\p{L}\p{N}])")
        self.assertEqual(piano_match.source("피아노 솔로"), r"피아노\s*솔로")
        self.assertEqual(piano_match.source("連弾"), "連弾")
        self.assertEqual(piano_match.source("a.b"), r"(?<![\p{L}\p{N}])a\.b(?![\p{L}\p{N}])")
        # A JavaScript "u" regex rejects needless escapes, so "-" stays bare.
        self.assertNotIn("\\-", piano_match.rules_json())
        self.assertTrue(re.compile(piano_match.source("lo-fi", piano_match.PY_LETTER)).search("lo-fi"))


if __name__ == "__main__":
    unittest.main()
