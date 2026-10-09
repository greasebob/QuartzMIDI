// rules_test.js: the page's matcher (QMParse.isOther, hasPiano, isRecording) on the same fixtures and cases as
// test_piano_match.py, with the rules loaded from fixtures/rules.json as the page loads /quartzmidi/rules.json.
//   deno test --allow-read tools\mp3-to-midi\tests
import "../page/parse.js";

const P = globalThis.QMParse;
const fixture = (name) => JSON.parse(Deno.readTextFileSync(new URL("./fixtures/" + name, import.meta.url)));
const RULES = fixture("rules.json");
P.loadRules(RULES);

function check(cond, msg) {
  if (!cond) throw new Error(msg);
}
const other = (title, query, want) =>
  check(P.isOther(title, query) === want, "isOther(" + JSON.stringify(title) + ", " + JSON.stringify(query) + ") should be " + want);

Deno.test("titles fixture", () => {
  const cases = fixture("titles.json");
  check(cases.length > 50, "titles.json has " + cases.length + " cases");
  const wrong = cases.filter((c) => P.isOther(c.title, c.query) !== c.expected);
  check(wrong.length === 0, "wrong: " + JSON.stringify(wrong));
});

Deno.test("the fixture covers every term", () => {
  // The same terms as the Python test leaves to the unit cases below.
  const skip = new Set(["orchestra", "symphony", "ensemble", "viola", "flute", "clarinet", "sax", "trumpet", "harp",
    "guitar", "drum", "vocal", "singing", "sings", "sing along", "acapella", "react", "연탄", "바이올린", "小提琴"]);
  const titles = fixture("titles.json").filter((c) => c.expected).map((c) => c.title.normalize("NFKC").toLowerCase());
  for (const entry of [...RULES.other, ...RULES.lesson]) {
    if (skip.has(entry.term)) continue;
    const re = new RegExp(entry.source, entry.flags);
    check(titles.some((t) => re.test(t)), "no title hits " + entry.term);
  }
});

Deno.test("rules shape", () => {
  check(RULES.normalize === "NFKC" && RULES.lowercase === true, "normalize and lowercase");
  const unless = RULES.other.flatMap((e) => e.unless || []);
  for (const entry of [...RULES.solo, ...RULES.other, ...RULES.lesson, ...unless]) {
    check(entry.flags === "u", entry.term + ": flags " + entry.flags + " (never g or y: test() must not keep state)");
    new RegExp(entry.source, entry.flags);
  }
  for (const entry of [...RULES.solo, ...RULES.lesson, ...unless]) {
    check(!("unless" in entry), entry.term + ": only an other term has exceptions");
  }
  check(JSON.stringify(P.PIANO_WORDS) === JSON.stringify(RULES.piano), "PIANO_WORDS differs from rules.json piano");
  check(JSON.stringify(P.LESSON) === JSON.stringify(RULES.lesson.map((e) => e.term)), "LESSON differs from rules.json lesson");
});

Deno.test("recordings fixture", () => {
  // The same 80 hand-labelled titles and limits as test_piano_match.py: misses the titles cannot show (a trio,
  // a live ensemble, a compilation) and solo recordings with chapters or "with lyrics", and no more of them. The
  // fixture keeps only a count of chapters; each untitled one is its own piece.
  const cases = fixture("recordings.json");
  check(cases.length === 80, "recordings.json has " + cases.length + " titles");
  const row = (c) => ({ kind: "video", id: "aaaaaaaaaaa", title: c.title, channel: c.channel, seconds: c.seconds,
    chapters: Array.from({ length: c.chapters }, (_, i) => ({ title: "", seconds: i })), playable: null, live: false,
    upcoming: false });
  const counted = cases.filter((c) => P.isRecording(row(c), c.query) && !c.recording).map((c) => c.title);
  const missed = cases.filter((c) => c.recording && !P.isRecording(row(c), c.query)).map((c) => c.title);
  check(counted.length <= 3, "counted, not recordings: " + JSON.stringify(counted));
  check(missed.length <= 8, "recordings missed: " + JSON.stringify(missed));
});

Deno.test("Latin terms need non-letters around them", () => {
  other("Violin Sonata", "", true);
  other("violin.", "", true);
  other("(violin)", "", true);
  other("piano_violin", "", true); // "_" is not a letter
  other("Violinist plays Chopin", "", false);
  other("Harpsichord Suite", "", false);
  other("Saxony", "", false);
  other("Flute 2", "", true);
  other("Flute2", "", false); // a digit counts as a letter here
  other("Ｓａｘ", "", true); // NFKC folds full-width letters
});

Deno.test("CJK terms match as substrings", () => {
  for (const title of ["千本桜バイオリンとピアノ", "봄날바이올린", "梁祝小提琴协奏曲", "ピアノ連弾", "피아노연탄", "ﾊﾞｲｵﾘﾝ"]) {
    other(title, "", true);
  }
});

Deno.test("solo wins outright", () => {
  for (const title of ["Piano Solo with Orchestra", "solo piano - violin cover", "ピアノソロ 連弾", "피아노 솔로 바이올린",
    "피아노솔로 바이올린", "钢琴独奏 小提琴", "鋼琴獨奏 大提琴", "ピアノ独奏 合唱"]) {
    other(title, "", false);
  }
  other("Solo Pianist with Orchestra", "", true);
});

Deno.test("query exemption", () => {
  other("Canon - Violin & Piano", "", true);
  other("Canon - Violin & Piano", "canon violin", false);
  other("Canon - Violin & Piano", "ＶＩＯＬＩＮ", false);
  other("Canon - Violin & Cello", "violin", true);
  other("Canon - Violin & Piano", "violinist", true);
  other("千本桜 連弾", "連弾 千本桜", false);
  other("Drums and Piano", "drums", false);
  other("Drums and Piano", "drum", true); // each term on its own
  other("Piano Duet", "duet", true);
  other("Piano Duet", "piano duet", false);
});

Deno.test("concerto unless solo", () => {
  check(RULES.other.some((e) => e.term === "concerto"), "concerto is not in rules.json");
  other("Rachmaninoff - Piano Concerto No. 2", "", true);
  other("Concerto No. 2 for solo piano", "", false);
  other("Rachmaninoff - Piano Concerto No. 2", "rachmaninoff concerto", false);
  // Solo by nature, or a solo arrangement; an orchestra or another instrument named as well still counts.
  for (const title of ["Italian Concerto", "Bach - Concerto in D minor, BWV 974", "Bach - Concerto, BWV987",
    "Alkan - Concerto, Op. 39", "Concerto pour piano seul", "Concerto (Solo Version)", "Concerto #2 minus orchestra",
    "Concerto without Orchestra", "Concerto (Piano Arrangement)", "Concerto arr. for piano", "Concerto 2 (Stride Piano)"]) {
    other(title, "", false);
  }
  for (const title of ["Bach - Concerto in A minor, BWV 1065", "Bach - Concerto, BWV 1052", "Bach - Concerto, BWV 9710",
    "Alkan - Concerto da camera No. 2", "Concerto (Solo) with Orchestra", "Italian Concerto - Violin",
    "Italian Concerto - Piano Tutorial", "Orchestra minus piano"]) {
    other(title, "", true);
  }
});

Deno.test("unless terms take back their term only", () => {
  for (const entry of RULES.other.filter((e) => e.unless)) {
    for (const back of entry.unless) {
      other("Song " + entry.term, "", true);
      other("Song " + entry.term + " " + back.term, "", false);
      other("Song " + entry.term + " " + back.term + " cello", "", true);
      other("Song " + entry.term + " " + back.term + " tutorial", "", true);
    }
  }
  check(RULES.other.filter((e) => e.unless).map((e) => e.term).join() === "orchestra,concerto", "terms with exceptions");
});

Deno.test("lesson terms", () => {
  for (const term of P.LESSON) {
    other("Song - " + term, "", true);
    other("Song - " + term, "song " + term, false);
  }
});

Deno.test("solo does not save a lesson", () => {
  // SOLO overrides OTHER only.
  for (const title of ["Nocturne - Piano Solo Tutorial", "solo piano lesson", "ピアノソロ 弾き方", "피아노 솔로 강좌", "钢琴独奏 教学"]) {
    other(title, "", true);
  }
  other("Piano Solo Tutorial with Violin", "tutorial", false);
  other("Piano Tutorial with Violin", "tutorial", true);
});

Deno.test("lesson query exemption", () => {
  other("Für Elise - Piano Tutorial", "für elise tutorial", false);
  other("Für Elise - Piano Tutorial", "ＴＵＴＯＲＩＡＬ", false);
  other("Für Elise - Piano Tutorial", "tutorials", true); // each term on its own
  other("How to Play Für Elise", "how to", true);
  other("ピアノ講座 第1回", "講座", false);
  other("Easy Piano Tutorial", "easy piano", true); // the query does not ask for "tutorial"
});

Deno.test("left out", () => {
  // These name performances too, so they never dim a title.
  const terms = [...RULES.other, ...RULES.lesson].map((e) => e.term);
  for (const word of ["synthesia", "sheet music", "medley", "learn", "vs", "challenge", "chords", "left hand", "right hand",
    "step by step", "beginner", "how to", "101"]) {
    check(!terms.includes(word), word + " is in rules.json");
    other(word + " - Song", "", false);
  }
});

Deno.test("isRecording: counted and a minute or longer", () => {
  const row = (f = {}) => ({ kind: "video", id: "aaaaaaaaaaa", title: "Nocturne", seconds: 300, chapters: [], playable: null,
    live: false, upcoming: false, ...f });
  const rec = (r, query, want) => check(P.isRecording(r, query) === want, "isRecording(" + JSON.stringify(r) + ") should be " + want);
  rec(row(), "", true);
  rec(row({ seconds: 59 }), "", false);
  rec(row({ seconds: 60 }), "", true);
  rec(row({ seconds: 1200 }), "", true);
  rec(row({ seconds: 1201 }), "", false);
  rec(row({ seconds: null }), "", false);
  rec(row({ chapters: [{ title: "Nocturne", seconds: 0 }, { title: "Waltz", seconds: 100 }] }), "", false);
  rec(row({ chapters: [{ title: "Intro", seconds: 0 }, { title: "Verse", seconds: 20 }, { title: "Chorus", seconds: 60 }] }), "",
    true);
  rec(row({ live: true }), "", false);
  rec(row({ playable: false }), "", false);
  rec(row({ members: true }), "", false);
  rec({ ...row(), kind: "playlist" }, "", false);
  rec(row({ title: "Canon - Violin & Piano" }), "", false);
  rec(row({ title: "Canon - Violin & Piano" }), "canon violin", true);
  rec(row({ title: "Nocturne - Piano Tutorial" }), "", false);
  rec(row({ title: "Nocturne - Piano Tutorial" }), "nocturne tutorial", true);
  rec(null, "", false);
});

Deno.test("lessonChannel", () => {
  for (const name of ["Jazz Tutorial", "Piano Tutorials", "Jazz Lesson Videos", "London Contemporary School of Piano",
    "The Keys Coach", "Learn Piano with Jazer Lee", "Piano Academy", "Your Piano Teacher", "Beginner Piano Course",
    "Piano Skills", "Beginners Piano", "ＰＩＡＮＯ ＬＥＳＳＯＮＳ", "ピアノ講座", "ヤマハ音楽教室", "ピアノレッスン", "피아노 강좌", "피아노 레슨",
    "钢琴教学", "钢琴教程"]) {
    check(P.lessonChannel(name) === true, "lessonChannel(" + JSON.stringify(name) + ") should be true");
  }
  for (const name of ["Rousseau", "Kassia", "Animenz Piano Sheets", "Jacob Koller / The Mad Arranger", "Learning Piano",
    "Schoolhouse Rock", "Skillset", "Kaleb Brasee - Topic", "", null, undefined]) {
    check(P.lessonChannel(name) === false, "lessonChannel(" + JSON.stringify(name) + ") should be false");
  }
});

Deno.test("band, bass, duet and lyrics count only as phrases", () => {
  for (const title of ["Band of Brothers", "All About That Bass", "Duet", "Lyrics", "The Band - Piano", "Bass Drop Piano", "Lyric Piano"]) {
    other(title, "", false);
  }
  for (const title of ["full band", "band cover", "bass cover", "bass guitar", "piano duet", "duet ver.", "lyric video",
    "with lyrics", "lyrics video"]) {
    other("Song - " + title, "", true);
  }
});

Deno.test("the added terms", () => {
  const terms = RULES.other.map((e) => e.term);
  for (const term of ["voice", "strings", "organ", "lofi", "lo-fi", "remix", "4-hands", "four hands", "2 pianos",
    "piano duo", "trio", "accordion", "ukulele", "kalimba", "marimba", "music box", "連弾", "四手", "연탄", "合唱",
    "歌ってみた", "弾き語り", "歌詞", "バイオリン", "ヴァイオリン", "바이올린", "小提琴", "チェロ", "大提琴", "ギター", "吉他",
    "オーケストラ", "管弦楽", "交响", "오케스트라", "二胡", "古箏"]) {
    check(terms.includes(term), term + " is not in rules.json");
    other("Song " + term + " cover", "", true);
  }
});

Deno.test("the current terms stay", () => {
  for (const term of ["orchestra", "orchestral", "symphony", "ensemble", "quartet", "4 hands", "four hands", "two pianos",
    "violin", "viola", "cello", "flute", "clarinet", "sax", "saxophone", "trumpet", "harp", "guitar", "drum", "drums",
    "vocal", "vocals", "singing", "sings", "sing along", "choir", "karaoke", "acapella", "a cappella", "react", "reacts",
    "reaction"]) {
    other("Song " + term, "", true);
  }
});

Deno.test("spaces in terms", () => {
  other("Piano  Four   Hands", "", true);
  other("Nocturne (Piano　Solo) Violin", "", false); // NFKC turns U+3000 into a space
  other("피아노솔로 바이올린", "", false);
});

Deno.test("odd input", () => {
  other("", "", false);
  other(null, "", false);
  other(undefined, undefined, false);
  other("Violin", null, true);
});

Deno.test("a rule tested twice gives the same answer", () => {
  // A g or y flag would make test() resume from lastIndex; loadRules strips them.
  P.loadRules({ ...RULES, other: [{ term: "violin", source: "violin", flags: "gu" }] });
  try {
    other("violin", "", true);
    other("violin", "", true);
  } finally {
    P.loadRules(RULES);
  }
});

Deno.test("hasPiano", () => {
  for (const q of ["solo piano", "PIANO", "ＰＩＡＮＯ", "pianist", "Pianos", "pianista", "千本桜 ピアノ", "ﾋﾟｱﾉ", "봄날 피아노",
    "卡农 钢琴", "卡農 鋼琴"]) {
    check(P.hasPiano(q) === true, "hasPiano(" + JSON.stringify(q) + ") should be true");
  }
  for (const q of ["", null, undefined, "chopin nocturne", "pian", "keyboard"]) {
    check(P.hasPiano(q) === false, "hasPiano(" + JSON.stringify(q) + ") should be false");
  }
});
