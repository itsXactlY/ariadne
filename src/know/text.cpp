// src/know/text.cpp — impl of dd/know/text.hpp.
#include "dd/know/text.hpp"

#include "dd/know/scan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>

namespace dd {
namespace know {

namespace {

// ---- UTF-8 ----

// Decodes one code point at s[i]; advances i. A broken byte is its own code point.
uint32_t next_cp(std::string_view s, size_t& i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = n == 3 ? (c & 0x07u) : n == 2 ? (c & 0x0Fu) : n == 1 ? (c & 0x1Fu) : c;
    ++i;
    while (n-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3Fu);
    return cp;
}

// The folded spelling of a non-ASCII letter, or nullptr when it is kept as it is.
const char* fold_cp(uint32_t cp) {
    switch (cp) {
        case 0xC4: case 0xE4: return "ae";
        case 0xD6: case 0xF6: return "oe";
        case 0xDC: case 0xFC: return "ue";
        case 0xDF: return "ss";
        case 0xC6: case 0xE6: return "ae";
        case 0x152: case 0x153: return "oe";
        case 0xC7: case 0xE7: return "c";
        case 0xD1: case 0xF1: return "n";
        case 0xD8: case 0xF8: return "o";
        case 0xDD: case 0xFD: case 0xFF: return "y";
        default: break;
    }
    if ((cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xE0 && cp <= 0xE5)) return "a";
    if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB)) return "e";
    if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF)) return "i";
    if ((cp >= 0xD2 && cp <= 0xD5) || (cp >= 0xF2 && cp <= 0xF5)) return "o";
    if ((cp >= 0xD9 && cp <= 0xDB) || (cp >= 0xF9 && cp <= 0xFB)) return "u";
    return nullptr;
}

// Symbols, punctuation, arrows, box drawing, emoji: a word break, never part of a word.
bool symbol_cp(uint32_t cp) {
    if (cp < 0x80) return false;
    if (cp < 0xC0) return true;                          // Latin-1 punctuation and symbols
    if (cp == 0xD7 || cp == 0xF7) return true;           // x /
    if (cp >= 0x2000 && cp < 0x3000) return true;        // punctuation, arrows, math, box drawing, dingbats
    if (cp >= 0x3000 && cp < 0x3040) return true;        // CJK punctuation
    if (cp >= 0xFE00 && cp < 0xFE10) return true;        // variation selectors
    if (cp >= 0x1F000) return true;                      // emoji and pictographs
    return false;
}

// ---- stopwords, one table per language ----
// English and German are the default (config knowledge.stopword_languages, see set_stopword_languages). The other
// tables are opt-in: a stopword list is one language's, and a word that is a stopword there is often a content word
// in another or in code. Words are kept in their own spelling; the table build folds them as the tokenizer does.

// en
const char* const kStopEn[] = {
    "a", "about", "above", "after", "again", "all", "also", "am", "an", "and", "any", "are", "as", "at", "be",
    "because", "been", "before", "being", "below", "between", "both", "but", "by", "can", "could", "did", "do",
    "does", "doing", "down", "during", "each", "few", "for", "from", "further", "had", "has", "have", "having",
    "he", "her", "here", "hers", "him", "his", "how", "if", "in", "into", "is", "it", "its", "itself", "just",
    "me", "more", "most", "my", "myself", "no", "nor", "not", "now", "of", "off", "on", "once", "only", "or",
    "other", "our", "ours", "out", "over", "own", "same", "she", "should", "so", "some", "such", "than",
    "that", "the", "their", "theirs", "them", "then", "there", "these", "they", "this", "those", "through",
    "to", "too", "under", "until", "up", "very", "was", "we", "were", "what", "when", "where", "which",
    "while", "who", "whom", "why", "will", "with", "would", "you", "your", "yours", "yourself", "ok", "okay",
    "yes", "yeah", "lol", "please", "thanks", "thank", "get", "got", "let", "lets", "make", "want", "need",
    "like", "really", "still", "even", "much", "many", "one", "two", "use", "using", "used", "maybe",
    "actually", "basically", "already", "always", "never", "ever", "another", "every", "everything",
    "something", "anything", "dont", "doesnt", "didnt", "isnt", "arent", "wasnt", "cant", "wont", "im", "ive",
    "ill", "youre", "thats", "theres", "whats", "wtf", "ffs", "omg", "idk", "imo", "btw", "fyi", "tbh", "smh",
    "ugh", "hmm", "yep", "nope", "gonna", "wanna"};

// de
const char* const kStopDe[] = {
    "aber", "alle", "allem", "allen", "aller", "alles", "als", "also", "am", "an", "ander", "andere",
    "anderen", "auch", "auf", "aus", "bei", "bin", "bis", "bist", "da", "dadurch", "dafuer", "fuer", "dagegen",
    "daher", "damit", "danach", "dann", "das", "dass", "daran", "darauf", "darin", "darum", "davon", "dazu",
    "dein", "deine", "deinen", "deiner", "dem", "den", "denn", "der", "des", "dessen", "deshalb", "die",
    "dies", "diese", "diesem", "diesen", "dieser", "dieses", "doch", "dort", "du", "durch", "ein", "eine",
    "einem", "einen", "einer", "eines", "einmal", "er", "es", "etwas", "euch", "euer", "gegen", "gewesen",
    "hab", "habe", "haben", "hast", "hat", "hatte", "hatten", "hin", "hinter", "ich", "ihm", "ihn", "ihnen",
    "ihr", "ihre", "ihrem", "ihren", "ihrer", "im", "indem", "ins", "ist", "ja", "jede", "jedem", "jeden",
    "jeder", "jedes", "jetzt", "kann", "kannst", "kein", "keine", "keinen", "keiner", "koennen", "koennte",
    "machen", "mal", "man", "mehr", "mein", "meine", "meinen", "meiner", "mich", "mir", "mit", "muss", "musst",
    "nach", "nein", "nicht", "nichts", "noch", "nun", "nur", "ob", "oder", "ohne", "sehr", "sein", "seine",
    "seinen", "seiner", "sich", "sie", "sind", "so", "soll", "sollen", "sollte", "sonst", "ueber", "um", "und",
    "uns", "unser", "unsere", "unter", "vom", "von", "vor", "waehrend", "war", "waren", "warum", "was",
    "wegen", "weil", "weiter", "welche", "wenn", "wer", "werde", "werden", "wie", "wieder", "will", "wir",
    "wird", "wirst", "wo", "woher", "wohin", "wollen", "wurde", "wurden", "zu", "zum", "zur", "zwischen",
    "halt", "eben", "schon", "gerade", "bitte", "danke", "ne", "nee", "jo", "ach", "oh", "hm", "irgendwie",
    "irgend", "ganz", "gar", "bloss", "kaum"};

// nl
const char* const kStopNl[] = {
    "de", "het", "een", "en", "van", "in", "op", "te", "dat", "die", "dit", "is", "was", "zijn", "waren",
    "worden", "werd", "ik", "jij", "je", "u", "hij", "zij", "ze", "wij", "we", "jullie", "hun", "hen", "er",
    "daar", "hier", "maar", "of", "om", "voor", "met", "naar", "bij", "uit", "aan", "als", "ook", "nog", "wel",
    "niet", "geen", "heeft", "hebben", "kan", "kunnen", "zou", "zouden", "over", "onder"};

// fr
const char* const kStopFr[] = {
    "le", "la", "les", "un", "une", "des", "du", "de", "et", "ou", "mais", "donc", "car", "ni", "que", "qui",
    "quoi", "dont", "où", "ce", "cet", "cette", "ces", "ça", "cela", "ceci", "il", "elle", "ils", "elles",
    "on", "nous", "vous", "je", "tu", "me", "te", "se", "mon", "ma", "mes", "ton", "ta", "tes", "son", "sa",
    "ses", "notre", "votre", "leur", "leurs", "dans", "sur", "sous", "avec", "sans", "pour", "par", "vers",
    "chez", "entre", "comme", "tout", "tous", "toute", "toutes", "être", "avoir", "est", "sont", "était",
    "été", "avait", "ont", "fait", "faire", "peut", "peu", "plus", "moins", "très", "bien", "aussi", "pas",
    "ne", "jamais", "rien", "personne", "ici", "là", "quand", "comment", "pourquoi", "oui", "non"};

// es
const char* const kStopEs[] = {
    "el", "la", "los", "las", "un", "una", "unos", "unas", "y", "o", "pero", "sino", "porque", "que", "qué",
    "quien", "quién", "cual", "cualquiera", "cuando", "cuándo", "donde", "dónde", "como", "cómo", "yo", "tú",
    "usted", "ustedes", "él", "ella", "ellos", "ellas", "nosotros", "nosotras", "vosotros", "vosotras", "me",
    "te", "se", "nos", "os", "mi", "mis", "tu", "tus", "su", "sus", "nuestro", "nuestra", "vuestro", "vuestra",
    "lo", "le", "les", "del", "al", "en", "con", "sin", "por", "para", "sobre", "entre", "hasta", "desde",
    "es", "son", "era", "eran", "ser", "estar", "está", "están", "estaba", "haber", "hay", "ha", "han", "he",
    "hemos", "tiene", "tienen", "tener", "puede", "pueden", "muy", "más", "menos", "también", "ya", "aquí",
    "ahí", "allí", "todo", "todos", "nada", "algo", "alguien", "nadie", "sí", "no", "bien", "así"};

// pt
const char* const kStopPt[] = {
    "o", "a", "os", "as", "um", "uma", "uns", "umas", "e", "ou", "mas", "porque", "que", "quem", "qual",
    "quais", "quando", "onde", "como", "eu", "tu", "você", "vocês", "ele", "ela", "eles", "elas", "nós", "nos",
    "me", "te", "se", "lhe", "lhes", "meu", "minha", "seu", "sua", "nosso", "nossa", "isto", "isso", "aquilo",
    "este", "esta", "esse", "essa", "aquele", "aquela", "do", "da", "dos", "das", "no", "na", "nas", "ao",
    "aos", "à", "às", "de", "em", "por", "para", "com", "sem", "sobre", "entre", "até", "ser", "estar", "ter",
    "haver", "sou", "são", "foi", "era", "está", "estão", "tem", "têm", "há", "muito", "mais", "menos",
    "também", "já", "ainda", "aqui", "ali", "tudo", "nada", "sim", "não"};

// it
const char* const kStopIt[] = {
    "il", "lo", "la", "i", "gli", "le", "un", "uno", "una", "e", "ed", "o", "ma", "però", "che", "chi", "cui",
    "quale", "quali", "quando", "dove", "come", "perché", "io", "tu", "lui", "lei", "noi", "voi", "loro", "mi",
    "ti", "si", "ci", "vi", "ne", "mio", "mia", "miei", "mie", "tuo", "suo", "nostro", "vostro", "questo",
    "questa", "quello", "quella", "del", "dello", "della", "dei", "degli", "delle", "al", "allo", "alla",
    "agli", "alle", "nel", "nella", "nei", "nelle", "da", "di", "a", "in", "con", "su", "per", "tra", "fra",
    "essere", "avere", "sono", "sei", "era", "erano", "è", "ha", "hanno", "ho", "abbiamo", "fare", "può",
    "molto", "più", "meno", "anche", "già", "ancora", "qui", "lì", "tutto", "niente", "qualcosa", "qualcuno",
    "nessuno", "sì", "no"};

// ca
const char* const kStopCa[] = {
    "els", "les", "un", "una", "uns", "unes", "i", "o", "però", "perquè", "què", "qui", "quan", "on", "com",
    "jo", "tu", "ell", "ella", "nosaltres", "vosaltres", "ells", "elles", "amb", "sense", "per", "del", "dels",
    "al", "als", "és", "són", "era", "estar", "ser", "tenir", "molt", "més", "menys", "també", "això", "allò",
    "aquest", "aquesta", "aquests", "aquestes"};

// gl
const char* const kStopGl[] = {
    "non", "unha", "uns", "unhas", "e", "mais", "porque", "quen", "onde", "cando", "eu", "ti", "el", "ela",
    "nós", "vós", "eles", "elas", "con", "sen", "para", "por", "desde", "ata", "está", "están", "foi", "son",
    "ten", "teñen", "moi", "tamén", "aínda"};

// ro
const char* const kStopRo[] = {
    "și", "sau", "dar", "iar", "că", "ce", "cine", "care", "când", "unde", "cum", "de", "din", "la", "în",
    "pe", "cu", "fără", "pentru", "prin", "spre", "sub", "peste", "eu", "tu", "el", "ea", "noi", "voi", "ei",
    "ele", "mă", "te", "se", "ne", "vă", "îl", "îi", "le", "un", "o", "niște", "acest", "această", "acești",
    "aceste", "cel", "cea", "cei", "cele", "este", "sunt", "era", "erau", "fost", "fi", "avea", "are", "am",
    "ai", "au", "foarte", "mai", "mult", "puțin", "tot", "toate", "nimic", "da", "nu"};

// sv
const char* const kStopSv[] = {
    "och", "att", "det", "den", "de", "dem", "en", "ett", "som", "är", "var", "vara", "har", "hade", "kan",
    "kunde", "ska", "skall", "skulle", "inte", "ingen", "inget", "inga", "men", "eller", "om", "för", "från",
    "med", "utan", "till", "på", "av", "i", "över", "under", "efter", "före", "vid", "hos", "jag", "du", "han",
    "hon", "vi", "ni", "oss", "er", "sin", "sitt", "sina", "min", "mitt", "mina", "din", "ditt", "dina",
    "hans", "hennes", "deras", "här", "där", "nu", "då", "när", "hur", "vad", "vem", "vilken", "vilket",
    "alla", "allt", "någon", "något", "några"};

// no
const char* const kStopNo[] = {
    "ikke", "og", "det", "den", "jeg", "du", "han", "hun", "vi", "dere", "de", "som", "er", "var", "har",
    "hadde", "kan", "kunne", "skal", "skulle", "ingen", "intet", "men", "eller", "om", "for", "fra", "med",
    "uten", "til", "på", "av", "i", "over", "under", "etter", "før", "ved", "her", "der", "nå", "da", "hva",
    "hvem", "hvor", "hvordan", "hvilken", "alle", "alt", "noen", "noe"};

// da
const char* const kStopDa[] = {
    "og", "at", "det", "den", "de", "en", "et", "som", "er", "var", "være", "har", "havde", "kan", "kunne",
    "skal", "skulle", "ikke", "ingen", "intet", "men", "eller", "om", "for", "fra", "med", "uden", "til",
    "på", "af", "i", "over", "under", "efter", "før", "jeg", "du", "han", "hun", "vi", "I", "her", "der", "nu",
    "hvad", "hvem", "hvor", "hvordan", "alle", "alt", "nogen", "noget"};

// fi
const char* const kStopFi[] = {
    "ja", "tai", "että", "joka", "jotka", "mikä", "mitä", "missä", "milloin", "miten", "minä", "sinä", "hän",
    "me", "te", "he", "minun", "sinun", "hänen", "meidän", "teidän", "heidän", "on", "ovat", "oli", "olivat",
    "olla", "olen", "olet", "emme", "ette", "eivät", "ei", "en", "et", "ettei", "se", "ne", "tämä", "tuo",
    "nämä", "nuo", "joku", "jokin", "kaikki", "jotain", "mitään", "vain", "myös", "vielä", "jo", "nyt",
    "hyvin", "paljon", "vähän"};

// et
const char* const kStopEt[] = {
    "ning", "või", "et", "see", "seda", "need", "mis", "kes", "kuidas", "kus", "millal", "mina", "sina",
    "tema", "meie", "teie", "nemad", "on", "oli", "olema", "olen", "oled", "oleme", "olete", "ei", "aga",
    "kui", "ka", "mitte", "kõik", "üks", "iga", "siin", "seal"};

// hu
const char* const kStopHu[] = {
    "és", "vagy", "de", "hogy", "mert", "mint", "aki", "ami", "amely", "én", "te", "ő", "mi", "ti", "ők",
    "engem", "téged", "őt", "minket", "titeket", "őket", "az", "ez", "ezt", "azt", "egy", "egyik", "másik",
    "van", "vannak", "volt", "voltak", "lesz", "lenne", "lehet", "kell", "nem", "igen", "is", "sem", "se",
    "már", "még", "nagyon", "csak", "minden", "mindig", "soha", "itt", "ott", "ide", "oda", "hogyha", "meg",
    "el", "fel", "le", "be", "ki", "át", "között", "alatt", "fölött", "után", "előtt", "nélkül", "szerint",
    "miatt"};

// pl
const char* const kStopPl[] = {
    "i", "oraz", "lub", "albo", "ale", "lecz", "że", "żeby", "aby", "bo", "ponieważ", "więc", "czy", "kto",
    "co", "który", "która", "które", "kiedy", "gdzie", "jak", "dlaczego", "ja", "ty", "on", "ona", "ono", "my",
    "wy", "oni", "one", "mnie", "mi", "mój", "moja", "moje", "twój", "twoja", "jego", "jej", "nasz", "wasz",
    "się", "sobie", "to", "ten", "ta", "te", "tamten", "taka", "takie", "jest", "są", "był", "była", "było",
    "byli", "być", "ma", "mają", "mieć", "może", "mogą", "nie", "tak", "bardzo", "więcej", "mniej", "już",
    "jeszcze", "tylko", "też", "również", "wszystko", "nic", "ktoś", "coś", "nikt", "nigdy", "zawsze", "tutaj",
    "tam", "dla", "do", "od", "na", "w", "we", "z", "ze", "za", "przez", "przy", "pod", "nad", "po", "przed",
    "między", "bez", "o"};

// cs
const char* const kStopCs[] = {
    "a", "i", "ani", "nebo", "ale", "že", "aby", "protože", "když", "co", "kdo", "který", "která", "které",
    "kde", "kdy", "jak", "proč", "já", "ty", "on", "ona", "ono", "my", "vy", "oni", "se", "si", "můj", "moje",
    "tvůj", "jeho", "její", "náš", "váš", "ten", "ta", "to", "tento", "tato", "toto", "je", "jsou", "byl",
    "byla", "bylo", "být", "má", "mají", "mít", "může", "mohou", "není", "nejsou", "ano", "ne", "tak", "velmi",
    "více", "méně", "už", "ještě", "jen", "také", "všechno", "nic", "někdo", "něco", "nikdo"};

// sk
const char* const kStopSk[] = {
    "a", "aj", "alebo", "ale", "že", "aby", "pretože", "keď", "čo", "kto", "ktorý", "ktorá", "ktoré", "kde",
    "kedy", "ako", "prečo", "ja", "ty", "on", "ona", "ono", "my", "vy", "oni", "sa", "si", "môj", "moja",
    "naš", "váš", "tento", "táto", "toto", "je", "sú", "bol", "bola", "bolo", "byť", "má", "majú", "mať",
    "môže", "nie", "áno", "veľmi", "viac", "menej", "už", "ešte", "len", "tiež", "všetko", "nič", "niekto",
    "niečo", "nikto"};

// sr
const char* const kStopSr[] = {
    "и", "или", "али", "да", "не", "се", "је", "су", "сам", "си", "смо", "сте", "био", "била", "било",
    "бити", "шта", "што", "ко", "који", "која", "које", "када", "где", "како", "зашто", "ја", "ти", "он",
    "она", "оно", "ми", "ви", "они", "оне", "ово", "то", "тај", "та", "те", "свој", "мој", "твој",
    "његов", "њен", "наш", "ваш", "њихов", "из", "од", "до", "за", "на", "у", "о", "по", "при", "са", "без",
    "између", "после", "пре", "овде", "тамо", "све", "нешто", "ништа", "неко", "нико", "веома", "више",
    "мање"};

// hr
const char* const kStopHr[] = {
    "in", "ali", "da", "ne", "se", "je", "su", "sam", "si", "smo", "ste", "što", "šta", "ko", "koji", "koja",
    "koje", "kada", "gdje", "gde", "kako", "zašto", "zasto", "ja", "ti", "on", "ona", "mi", "vi", "oni", "one",
    "ovo", "to", "taj", "ta", "svoj", "moj", "tvoj", "njegov", "njen", "naš", "vaš", "njihov", "iz", "od",
    "do", "za", "na", "u", "o", "po", "pri", "sa", "bez", "sve", "nešto", "ništa", "neko", "niko", "veoma",
    "više", "manje"};

// sl
const char* const kStopSl[] = {
    "in", "ali", "da", "ne", "se", "je", "so", "ker", "ki", "kaj", "kdo", "kateri", "katera", "katero", "kdaj",
    "kje", "kako", "jaz", "ti", "on", "ona", "mi", "vi", "oni", "moj", "tvoj", "njegov", "njen", "naš", "vaš",
    "njihov", "ta", "to", "sem", "si", "smo", "ste", "bil", "bila", "bilo", "biti", "ima", "imajo", "lahko",
    "zelo", "več", "manj", "že", "še", "tudi", "vse", "nič", "nekdo", "nekaj", "nihče"};

// bg
const char* const kStopBg[] = {
    "и", "а", "но", "или", "че", "да", "не", "на", "в", "с", "за", "от", "до", "по", "при", "под", "над",
    "между", "като", "ако", "когато", "където", "как", "кой", "коя", "кое", "кои", "аз", "ти", "той", "тя",
    "то", "ние", "вие", "те", "ме", "се", "си", "ми", "му", "ѝ", "им", "този", "тази", "това", "тези",
    "онзи", "онова", "съм", "е", "сме", "сте", "са", "беше", "били", "има", "имат", "може", "могат",
    "много", "малко", "още", "вече", "само", "също", "всичко", "нищо", "някой", "нещо", "никой"};

// mk
const char* const kStopMk[] = {
    "и", "или", "но", "дека", "што", "кој", "која", "кое", "кои", "кога", "каде", "како", "зошто", "јас",
    "ти", "тој", "таа", "тоа", "ние", "вие", "тие", "ме", "се", "си", "овој", "оваа", "ова", "овие", "е",
    "сум", "сме", "сте", "беше", "има", "имаат", "може", "не", "да", "многу", "малку", "уште", "веќе",
    "само", "исто", "сè", "ништо", "некој", "нешто", "никој"};

// ru
const char* const kStopRu[] = {
    "и", "а", "но", "или", "да", "нет", "не", "ни", "же", "ли", "что", "чтобы", "чем", "как", "кто",
    "кого", "кому", "кем", "который", "которая", "которое", "которые", "где", "куда", "откуда", "когда",
    "почему", "зачем", "я", "ты", "он", "она", "оно", "мы", "вы", "они", "меня", "мне", "мной", "тебя",
    "тебе", "его", "ему", "её", "ее", "ей", "им", "ими", "нас", "нам", "вас", "вам", "их", "себя", "себе",
    "мой", "моя", "моё", "мое", "мои", "твой", "наш", "ваш", "свой", "этот", "эта", "это", "эти", "тот",
    "та", "те", "здесь", "там", "тут", "сюда", "туда", "в", "во", "на", "к", "ко", "с", "со", "у", "о", "об",
    "от", "до", "из", "за", "для", "по", "при", "про", "без", "над", "под", "между", "через", "после",
    "перед", "около", "вокруг", "из-за", "из-под", "быть", "есть", "был", "была", "было", "были", "будет",
    "будут", "буду", "будем", "стал", "стала", "стали", "может", "могут", "нужно", "надо", "можно",
    "нельзя", "очень", "более", "менее", "уже", "ещё", "еще", "только", "также", "тоже", "всё", "все",
    "всего", "ничего", "никто", "кто-то", "что-то", "где-то", "когда-то", "иногда"};

// uk
const char* const kStopUk[] = {
    "і", "й", "та", "або", "але", "чи", "що", "щоб", "як", "хто", "кого", "кому", "ким", "який", "яка",
    "яке", "які", "де", "куди", "звідки", "коли", "чому", "навіщо", "я", "ти", "він", "вона", "воно", "ми",
    "ви", "вони", "мене", "мені", "мною", "тебе", "його", "її", "нам", "вам", "їм", "себе", "свій", "мій",
    "твій", "наш", "ваш", "цей", "ця", "це", "ці", "той", "те", "ті", "тут", "там", "сюди", "туди", "у", "в",
    "на", "до", "від", "з", "із", "за", "для", "по", "при", "під", "над", "між", "через", "після",
    "перед", "без", "є", "був", "була", "було", "були", "буде", "будуть", "може", "треба", "можна", "дуже",
    "більше", "менше", "вже", "ще", "тільки", "також", "теж", "все", "нічого", "ніхто"};

// be
const char* const kStopBe[] = {
    "і", "й", "ды", "або", "але", "што", "каб", "як", "хто", "дзе", "куды", "адкуль", "калі", "чаму", "я",
    "ты", "ён", "яна", "яно", "мы", "вы", "яны", "мяне", "мне", "цябе", "яго", "яе", "нас", "вас", "іх",
    "сябе", "мой", "твой", "наш", "ваш", "гэты", "гэта", "гэтыя", "той", "тая", "тое", "тут", "там", "у", "ў",
    "на", "да", "ад", "з", "за", "для", "па", "пры", "пад", "над", "паміж", "без", "ёсць", "быў", "была",
    "было", "былі", "будзе", "можа", "трэба", "можна", "вельмі", "больш", "менш", "ужо", "яшчэ",
    "толькі", "таксама", "усё", "нічога", "ніхто"};

// lt
const char* const kStopLt[] = {
    "ir", "ar", "bet", "kad", "kaip", "kas", "kur", "kada", "kodėl", "aš", "tu", "jis", "ji", "mes", "jūs",
    "jie", "jos", "mano", "tavo", "jo", "mūsų", "jūsų", "jų", "šis", "ši", "šitie", "tai", "tas", "ta", "tie",
    "tos", "yra", "buvo", "būti", "turi", "gali", "labai", "daug", "mažai", "jau", "dar", "tik", "taip", "ne",
    "viskas", "nieko", "kažkas", "niekas"};

// lv
const char* const kStopLv[] = {
    "un", "bet", "ka", "kas", "kur", "kad", "kāpēc", "es", "tu", "viņš", "viņa", "mēs", "jūs", "viņi",
    "viņas", "mans", "mana", "tavs", "tava", "mūsu", "jūsu", "tas", "tā", "šie", "šīs", "ir", "bija", "būt",
    "var", "nav", "ļoti", "daudz", "maz", "jau", "vēl", "tikai", "arī", "viss", "nekas", "kāds", "kaut",
    "nekad"};

// el
const char* const kStopEl[] = {
    "και", "ή", "αλλά", "ότι", "πως", "που", "ποιος", "ποια", "ποιο", "ποιοι", "ποιες", "πότε", "πού",
    "πώς", "γιατί", "εγώ", "εσύ", "αυτός", "αυτή", "αυτό", "εμείς", "εσείς", "αυτοί", "αυτές", "μου", "σου",
    "του", "της", "μας", "σας", "τους", "τις", "τα", "ο", "η", "το", "οι", "ένας", "μία", "ένα", "στο",
    "στη", "στην", "στον", "στους", "στις", "των", "τον", "την", "τι", "με", "σε", "από", "για", "ως",
    "προς", "χωρίς", "είναι", "ήταν", "έχει", "έχουν", "είχα", "είχαν", "θα", "δεν", "μη", "μην", "ναι",
    "όχι", "πολύ", "πιο", "λιγότερο", "ήδη", "ακόμα", "εδώ", "εκεί", "όλα", "κάποιος", "κάτι", "κανείς",
    "τίποτα"};

// sq
const char* const kStopSq[] = {
    "dhe", "ose", "por", "që", "se", "çfarë", "kush", "cili", "cila", "kur", "ku", "si", "pse", "unë", "ti",
    "ai", "ajo", "ne", "ju", "ata", "ato", "më", "të", "i", "e", "një", "ky", "kjo", "këta", "këto", "atë",
    "këtë", "është", "janë", "ishte", "ishin", "kam", "ke", "ka", "kemi", "keni", "kanë", "do", "mund", "nuk",
    "po", "jo", "shumë", "pak", "gjithçka", "asgjë", "dikush", "askush", "këtu", "atje", "nga", "për", "me",
    "pa", "në", "mbi", "nën", "pas", "para", "deri", "midis"};

// tr
const char* const kStopTr[] = {
    "ve", "veya", "ya", "ama", "fakat", "ancak", "çünkü", "ki", "de", "da", "mi", "mı", "mu", "mü", "ne",
    "kim", "hangi", "nasıl", "neden", "niçin", "nerede", "nereden", "nereye", "ben", "sen", "o", "biz", "siz",
    "onlar", "beni", "seni", "onu", "bizi", "sizi", "onları", "benim", "senin", "onun", "bizim", "sizin",
    "onların", "bu", "şu", "bunlar", "şunlar", "bunların", "şunların", "bir", "bazı", "her", "hiç", "tüm",
    "bütün", "çok", "az", "daha", "en", "kadar", "gibi", "için", "ile", "den", "dan", "ten", "tan", "değil",
    "var", "yok", "olmak", "olan", "olarak", "ise", "idi", "imiş", "oldu", "olacak", "olabilir", "burada",
    "orada", "şimdi", "sonra", "önce", "evet", "hayır"};

// az
const char* const kStopAz[] = {
    "və", "ya", "yaxud", "amma", "ancaq", "çünki", "ki", "bu", "o", "həmin", "mən", "sən", "biz", "siz",
    "onlar", "mənim", "sənin", "onun", "bizim", "sizin", "onların", "nə", "kim", "hansı", "necə", "harada",
    "hara", "üçün", "ilə", "kimi", "qədər", "var", "yox", "deyil", "olmaq", "idi", "isə", "çox", "az", "daha",
    "ən", "bütün", "hər", "heç", "burada", "orada", "indi", "sonra"};

// fa
const char* const kStopFa[] = {
    "و", "یا", "اما", "ولی", "که", "اگر", "چون", "زیرا", "برای", "از", "به", "با", "بی", "در", "بر", "تا",
    "را", "این", "آن", "اینها", "آنها", "هم", "همه", "هر", "هیچ", "چه", "کی", "کجا", "چرا", "چطور",
    "چگونه", "من", "تو", "او", "ما", "شما", "ایشان", "خود", "خودم", "خودت", "خودش", "است", "هست", "بود",
    "بودند", "هستند", "باشد", "شد", "شده", "می", "نمی", "کرد", "کرده", "کنند", "تواند", "باید", "شاید",
    "بسیار", "بیشتر", "کمتر", "فقط", "نیز", "همچنین", "حالا", "اکنون", "اینجا", "آنجا", "بله", "نه",
    "چیزی", "کسی", "هیچکس", "هیچ‌چیز"};

// ar
const char* const kStopAr[] = {
    "و", "أو", "أم", "لكن", "بل", "لأن", "أن", "إن", "إذا", "إذ", "ما", "ماذا", "من", "متى", "أين", "كيف",
    "لماذا", "هل", "قد", "لا", "لم", "لن", "ليس", "نعم", "أنا", "أنت", "أنتِ", "هو", "هي", "نحن", "أنتم",
    "أنتن", "هم", "هن", "هذا", "هذه", "هؤلاء", "ذلك", "تلك", "أولئك", "الذي", "التي", "الذين", "اللذان",
    "اللاتي", "كل", "بعض", "أي", "شيء", "أحد", "في", "على", "إلى", "عن", "مع", "منذ", "حتى", "بين", "تحت",
    "فوق", "أمام", "خلف", "بعد", "قبل", "عند", "لدى", "بسبب", "خلال", "كان", "كانت", "كانوا", "يكون",
    "تكون", "يكونون", "هناك", "هنا", "جدا", "كثيرا", "قليلا", "أيضا", "فقط", "دائما", "أبدا"};

// he
const char* const kStopHe[] = {
    "ו", "או", "אבל", "אלא", "כי", "אם", "כאשר", "ש", "מה", "מי", "מתי", "איפה", "איך", "למה", "האם", "אני",
    "אתה", "את", "הוא", "היא", "אנחנו", "אתם", "אתן", "הם", "הן", "זה", "זאת", "אלה", "אלו", "כל", "כמה",
    "הרבה", "מעט", "יותר", "פחות", "גם", "רק", "לא", "כן", "אין", "יש", "היה", "הייתה", "היו", "יהיה",
    "תהיה", "של", "על", "אל", "עם", "בלי", "בין", "מן", "מ", "ל", "ב", "כ", "ה", "פה", "שם", "כאן",
    "עכשיו", "מאוד"};

// hy
const char* const kStopHy[] = {
    "և", "ու", "կամ", "բայց", "որ", "եթե", "ինչ", "ով", "երբ", "որտեղ", "ինչպես", "ինչու", "ես", "դու", "նա",
    "մենք", "դուք", "նրանք", "իմ", "քո", "նրա", "մեր", "ձեր", "նրանց", "այս", "այդ", "այն", "բոլոր",
    "ամեն", "մի", "է", "են", "եմ", "ենք", "եք", "լինել", "կարող", "պետք", "չէ", "ոչ", "այո", "շատ", "քիչ",
    "ավելի", "նաև", "արդեն", "դեռ", "այստեղ", "այնտեղ", "հետ", "առանց", "մեջ"};

// ka
const char* const kStopKa[] = {
    "და", "ან", "მაგრამ", "რომ", "თუ", "როცა", "რაც", "ვინც", "რა", "ვინ", "სად", "როგორ", "რატომ", "მე",
    "შენ", "ის", "ჩვენ", "თქვენ", "ისინი", "ჩემი", "შენი", "მისი", "ჩვენი", "თქვენი", "მათი", "ეს", "ეგ",
    "ყველა", "ერთი", "არის", "იყო", "იქნება", "აქვს", "არა", "კი", "ძალიან", "მეტი", "ნაკლები", "აქ", "იქ",
    "ახლა", "უკვე", "კიდევ", "მხოლოდ", "სხვა", "ყველაფერი", "არაფერი"};

// kk
const char* const kStopKk[] = {
    "және", "немесе", "бірақ", "өйткені", "егер", "деп", "үшін", "мен", "сен", "сіз", "ол", "біз",
    "сендер", "сіздер", "олар", "кім", "не", "қайда", "қашан", "қалай", "неге", "бұл", "сол", "осы", "ана",
    "мына", "бар", "жоқ", "емес", "еді", "болды", "болып", "керек", "мүмкін", "өте", "көп", "аз", "тағы",
    "ғана", "бәрі", "ешкім", "ештеңе"};

// ky
const char* const kStopKy[] = {
    "жок", "жана", "бирок", "анткени", "эгер", "мен", "сен", "сиз", "ал", "биз", "силер", "сиздер", "алар",
    "ким", "эмне", "кайда", "качан", "кантип", "бул", "ошол", "ушул", "бар", "эмес", "болот", "болду",
    "керек", "мүмкүн", "өтө", "көп", "аз", "дагы", "гана", "баары"};

// uz
const char* const kStopUz[] = {
    "va", "yoki", "ammo", "lekin", "chunki", "agar", "ki", "bu", "shu", "u", "men", "sen", "siz", "biz",
    "ular", "mening", "sening", "uning", "bizning", "sizning", "ularning", "nima", "kim", "qaysi", "qayerda",
    "qachon", "qanday", "nega", "uchun", "bilan", "dan", "ga", "da", "bor", "yoq", "emas", "edi", "boladi",
    "kerak", "mumkin", "juda", "kop", "kam", "ham", "faqat", "barcha", "hech"};

// tk
const char* const kStopTk[] = {
    "we", "ýa", "emma", "ýöne", "sebäbi", "eger", "ki", "bu", "şu", "ol", "men", "sen", "siz", "biz", "olar",
    "näme", "kim", "haýsy", "nirede", "haçan", "nädip", "üçin", "bilen", "bar", "ýok", "däl", "bolýar",
    "gerek", "mümkin", "örän", "köp", "az"};

// mn
const char* const kStopMn[] = {
    "ба", "эсвэл", "гэхдээ", "учир", "хэрэв", "нь", "юм", "би", "чи", "та", "тэр", "бид", "тэд", "миний",
    "чиний", "түүний", "бидний", "тэдний", "юу", "хэн", "хаана", "хэзээ", "яаж", "яагаад", "энэ",
    "эдгээр", "тэдгээр", "бүх", "бүгд", "байна", "байсан", "байх", "болно", "үгүй", "тийм", "маш", "их",
    "бага", "бас", "зөвхөн", "одоо", "энд", "тэнд", "дараа", "өмнө"};

// hi
const char* const kStopHi[] = {
    "और", "या", "लेकिन", "कि", "क्योंकि", "अगर", "तो", "जो", "जिस", "जिसे", "क्या", "कौन", "कहाँ", "कब",
    "कैसे", "क्यों", "मैं", "तुम", "आप", "वह", "यह", "हम", "वे", "उन", "उनका", "उसका", "मेरा", "तेरा",
    "हमारा", "आपका", "इन", "इस", "उस", "एक", "को", "से", "में", "पर", "तक", "लिए", "द्वारा", "साथ",
    "बिना", "है", "हैं", "था", "थी", "थे", "हो", "होना", "कर", "करना", "रहा", "रही", "रहे", "गया", "गई",
    "गए", "नहीं", "हाँ", "जी", "भी", "ही", "बहुत", "कम", "अधिक", "सभी", "कुछ", "कोई"};

// ur
const char* const kStopUr[] = {
    "اور", "یا", "لیکن", "مگر", "کہ", "کیونکہ", "اگر", "تو", "جو", "کیا", "کون", "کہاں", "کب", "کیسے",
    "کیوں", "میں", "تم", "آپ", "وہ", "ہم", "یہ", "ان", "انکا", "اسکا", "میرا", "تیرا", "ہمارا", "آپکا",
    "ایک", "کو", "سے", "پر", "تک", "لیے", "ساتھ", "بغیر", "ہے", "ہیں", "تھا", "تھی", "تھے", "ہو", "ہونا",
    "کرنا", "نہیں", "ہاں", "بھی", "ہی", "بہت", "کم", "زیادہ", "سب", "کچھ"};

struct StopLang {
    const char* code;
    const char* const* words;
    size_t n;
};
template <size_t N>
constexpr StopLang lang(const char* code, const char* const (&words)[N]) {
    return {code, words, N};
}
const StopLang kLangs[] = {
    lang("en", kStopEn),
    lang("de", kStopDe),
    lang("nl", kStopNl),
    lang("fr", kStopFr),
    lang("es", kStopEs),
    lang("pt", kStopPt),
    lang("it", kStopIt),
    lang("ca", kStopCa),
    lang("gl", kStopGl),
    lang("ro", kStopRo),
    lang("sv", kStopSv),
    lang("no", kStopNo),
    lang("da", kStopDa),
    lang("fi", kStopFi),
    lang("et", kStopEt),
    lang("hu", kStopHu),
    lang("pl", kStopPl),
    lang("cs", kStopCs),
    lang("sk", kStopSk),
    lang("sr", kStopSr),
    lang("hr", kStopHr),
    lang("sl", kStopSl),
    lang("bg", kStopBg),
    lang("mk", kStopMk),
    lang("ru", kStopRu),
    lang("uk", kStopUk),
    lang("be", kStopBe),
    lang("lt", kStopLt),
    lang("lv", kStopLv),
    lang("el", kStopEl),
    lang("sq", kStopSq),
    lang("tr", kStopTr),
    lang("az", kStopAz),
    lang("fa", kStopFa),
    lang("ar", kStopAr),
    lang("he", kStopHe),
    lang("hy", kStopHy),
    lang("ka", kStopKa),
    lang("kk", kStopKk),
    lang("ky", kStopKy),
    lang("uz", kStopUz),
    lang("tk", kStopTk),
    lang("mn", kStopMn),
    lang("hi", kStopHi),
    lang("ur", kStopUr),
};

// Never a stopword through a foreign table: a word that is also an English word or a common tech token
// (ai ci io os var min lo see sin del su vi ...). en and de are their own tables and are taken as they are.
const char* const kProtected[] = {
    "agar", "ai", "al", "ale", "ali", "ama", "ami", "ammo", "ani", "ar", "ate", "az", "bar", "ben", "bet",
    "bien", "biz", "bolo", "ca", "car", "care", "chi", "ci", "co", "come", "comment", "con", "cos", "cox",
    "dan", "dar", "de", "del", "din", "dove", "ed", "el", "ell", "ella", "em", "emma", "en", "era",
    "est", "et", "fare", "fran", "ga", "ha", "ham", "han", "hans", "hay", "hen", "ho", "hun", "ii", "il", "io",
    "ir", "itt", "jag", "jane", "jest", "kane", "kim", "ku", "ky", "la", "len", "lo", "los", "ma", "mana",
    "mans", "mar", "mat", "meg", "men", "met", "mi", "min", "mint", "mitt", "molt", "mu", "non", "notre", "nu",
    "ok", "op", "os", "ott", "pa", "par", "pe", "per", "peste", "plus", "po", "pod", "pour", "sa", "sam",
    "sans", "sao", "se", "seal", "see", "sen", "sense", "shu", "sim", "sin", "sis", "son", "sou", "su", "sub",
    "sus", "ta", "take", "tam", "tan", "te", "ten", "ti", "tie", "til", "till", "ton", "tot", "tout", "un",
    "va", "vain", "van", "var", "vi", "vice", "vise", "volt", "wy"};

const std::unordered_set<std::string_view>& generic_path_words() {
    static const std::unordered_set<std::string_view> s = {
        "home", "usr", "var", "etc", "tmp", "opt", "bin", "sbin", "lib", "lib64", "share", "local", "src", "source",
        "sources", "include", "includes", "build", "builds", "dist", "target", "node_modules", "docs", "doc", "test",
        "tests", "examples", "example", "projects", "project", "repos", "repo", "code", "dev", "work", "workspace",
        "mnt", "media", "root", "proc", "sys", "run", "boot", "snap", "cache", "config", "data", "output", "out",
        "release", "debug", "main", "master", "pkg", "vendor", "third_party", "scripts", "script", "tools", "tool"};
    return s;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_alnum(char c) { return is_digit(c) || is_upper(c) || is_lower(c); }
char lower_of(char c) { return is_upper(c) ? static_cast<char>(c + 32) : c; }   // ASCII only, whatever the locale

// "0.1.45", "v1.2", "3.14.7-rc1"
bool version_shaped(std::string_view w) {
    size_t i = (w.size() > 1 && (w[0] == 'v' || w[0] == 'V') && is_digit(w[1])) ? 1 : 0;
    int dots = 0;
    bool digit_seen = false;
    for (; i < w.size(); ++i) {
        if (is_digit(w[i])) digit_seen = true;
        else if (w[i] == '.' && digit_seen) ++dots;
        else if ((w[i] == '-' || w[i] == '+') && dots >= 1) return true;
        else return false;
    }
    return dots >= 1 && digit_seen && w.back() != '.';
}

// ENOENT, SIGSEGV, ERR_CONNECTION, E0433
bool error_shaped(std::string_view w) {
    if (w.size() < 4) return false;
    if (w[0] == 'E' && std::all_of(w.begin() + 1, w.end(), is_digit) && w.size() <= 6) return true;
    int upper = 0;
    for (char c : w) {
        if (is_upper(c)) ++upper;
        else if (!(is_digit(c) || c == '_')) return false;
    }
    return upper >= 4;
}

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = lower_of(c);
    return s;
}

// Plural, -ing, -ed and a final e fold together (containers/container, deployed/deploy, caches/cache).
// Only plain lowercase words; both sides of a match go through the same function, so it need not be right, only the same.
std::string stem(std::string_view in) {
    if (in.size() < 4) return std::string(in);
    for (char c : in)
        if (!is_lower(c)) return std::string(in);
    std::string w(in);
    auto ends = [&](std::string_view suf) { return std::string_view(w).ends_with(suf); };
    auto undouble = [&] {
        const char c = w.back();
        if (w.size() >= 4 && w[w.size() - 2] == c && std::string_view("aeiouylsz").find(c) == std::string_view::npos) w.pop_back();
    };
    if (w.size() >= 5 && ends("ies")) {
        w.resize(w.size() - 3);
        w += "y";
    } else if (!ends("ss") && !ends("us") && !ends("is") && w.back() == 's') {
        w.pop_back();
    }
    if (w.size() >= 7 && ends("ing")) {
        w.resize(w.size() - 3);
        undouble();
    } else if (w.size() >= 6 && ends("ed")) {
        w.resize(w.size() - 2);
        undouble();
    }
    if (w.size() >= 5 && w.back() == 'e') w.pop_back();
    return w;
}

// The folded spelling the tokenizer would produce for a word, so a table written in a language's own spelling
// ("être", "Übung") matches: ASCII lowercased, accented Latin letters folded, other scripts as they are.
std::string fold_word(std::string_view w) {
    std::string o;
    for (size_t i = 0; i < w.size();) {
        const size_t at = i;
        const uint32_t cp = next_cp(w, i);
        if (cp < 0x80) o.push_back(lower_of(static_cast<char>(cp)));
        else if (const char* f = fold_cp(cp)) o += f;
        else o.append(w.substr(at, i - at));
    }
    return o;
}

// Whether the tokenizer can ever emit this word as a token: two bytes at least, ASCII only letters and digits, and no
// symbol or punctuation code point (those split a word).
bool producible(const std::string& w) {
    if (w.size() < 2) return false;
    for (size_t i = 0; i < w.size();) {
        const unsigned char c = static_cast<unsigned char>(w[i]);
        if (c < 0x80) {
            if (!(is_digit(w[i]) || is_lower(w[i]))) return false;
            ++i;
            continue;
        }
        if (symbol_cp(next_cp(w, i))) return false;
    }
    return true;
}

bool is_protected(const std::string& w) {
    return std::binary_search(std::begin(kProtected), std::end(kProtected), w.c_str(),
                              [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
}

std::string lang_code(std::string c) {
    for (auto& ch : c) ch = lower_of(ch);
    if (c == "nb" || c == "nn") return "no";
    if (c == "bs") return "hr";
    return c;
}

void add_language(std::set<std::string>& out, const StopLang& l) {
    const bool own = std::strcmp(l.code, "en") == 0 || std::strcmp(l.code, "de") == 0;   // taken as they are
    for (size_t i = 0; i < l.n; ++i) {
        std::string w = fold_word(l.words[i]);
        if (!own && (!producible(w) || is_protected(w))) continue;
        out.insert(std::move(w));
    }
}

std::set<std::string> words_for(const std::vector<std::string>& names) {
    std::set<std::string> out;
    for (const auto& n : names) {
        const std::string code = lang_code(n);
        for (const StopLang& l : kLangs)
            if (code == l.code) add_language(out, l);
    }
    return out;
}

// A stopword is short and there are few: the table is bucketed by first byte and length, so a lookup compares a
// handful of words instead of hashing the token.
struct StopTable {
    std::vector<std::string> store;                   // the folded words; the views below point into it
    std::vector<std::vector<std::string_view>> by;    // [first byte * 32 + length]
    std::unordered_set<std::string_view> big;         // 32 bytes and longer
    explicit StopTable(const std::set<std::string>& words) : store(words.begin(), words.end()), by(256 * 32) {
        for (const std::string& w : store) {
            if (w.empty()) continue;
            if (w.size() >= 32) big.insert(w);
            else by[static_cast<size_t>(static_cast<unsigned char>(w[0])) * 32 + w.size()].push_back(w);
        }
    }
    StopTable(const StopTable&) = delete;
    StopTable& operator=(const StopTable&) = delete;
    bool has(std::string_view w) const {
        if (w.empty()) return false;
        if (w.size() >= 32) return big.count(w) > 0;
        for (std::string_view x : by[static_cast<size_t>(static_cast<unsigned char>(w[0])) * 32 + w.size()])
            if (x == w) return true;
        return false;
    }
};

// The table in force: one pointer, swapped at setup. A replaced table is kept (a reader may still hold it): they are
// small and swaps are rare, so none is ever freed; the list keeps them reachable.
struct Retired {
    std::unique_ptr<const StopTable> table;
    Retired* next;
};
std::atomic<const StopTable*> g_stop{nullptr};
std::atomic<Retired*> g_retired{nullptr};

const StopTable& default_table() {
    static const StopTable t(words_for({"en", "de"}));
    return t;
}
const StopTable& stop_table() {
    const StopTable* p = g_stop.load(std::memory_order_acquire);
    return p ? *p : default_table();
}

// The bytes a word is trimmed of: before it ("(" "[" quotes ...) and after it (those and ". ! ?").
struct TrimSet {
    bool open[256] = {}, close[256] = {};
    constexpr TrimSet() {
        for (char c : std::string_view("()[]{}<>,;:\"'`")) open[static_cast<unsigned char>(c)] = close[static_cast<unsigned char>(c)] = true;
        for (char c : std::string_view(".!?")) close[static_cast<unsigned char>(c)] = true;
    }
};
constexpr TrimSet kTrim;

}  // namespace

bool is_stopword(std::string_view folded) { return stop_table().has(folded); }

void set_stopword_languages(const std::vector<std::string>& names) {
    const StopTable* t = new StopTable(words_for(names));
    auto* r = new Retired{std::unique_ptr<const StopTable>(t), g_retired.load(std::memory_order_relaxed)};
    while (!g_retired.compare_exchange_weak(r->next, r, std::memory_order_release, std::memory_order_relaxed)) {}
    g_stop.store(t, std::memory_order_release);
}

std::vector<std::string> stopword_languages() {
    std::vector<std::string> out;
    for (const StopLang& l : kLangs) out.push_back(l.code);
    return out;
}
bool is_generic_path_word(std::string_view lower) { return generic_path_words().count(lower) > 0; }

std::vector<std::string> terms_of(std::string_view text, bool keep_stop) {
    std::vector<std::string> out;
    const size_t n = text.size();
    out.reserve(n / 8 + 4);

    // a version or an error code is also a term as written. The words are the runs between blanks; a 64-byte
    // window at a time, the blanks come out of one classification as a mask, and a word is a view, not a copy.
    {
        auto word = [&](size_t b, size_t e) {
            while (b < e && kTrim.open[static_cast<unsigned char>(text[b])]) ++b;
            if (e - b < 3) return;   // no version or code is shorter
            const char c = text[b];
            if (!(is_digit(c) || is_upper(c) || c == '_' || c == 'v')) return;   // neither shape starts with another letter
            while (e > b && kTrim.close[static_cast<unsigned char>(text[e - 1])]) --e;
            const std::string_view w = text.substr(b, e - b);
            if (w.size() < 3) return;
            if (version_shaped(w) || error_shaped(w)) out.push_back(lower_ascii(std::string(w)));
        };
        size_t start = 0;
        bool in_word = false;
        for (size_t off = 0; off < n; off += 64) {
            const size_t len = std::min<size_t>(64, n - off);
            simd::Masks m;
            simd::classify(text.data() + off, len, &m);
            const uint64_t valid = len == 64 ? ~uint64_t{0} : (uint64_t{1} << len) - 1;
            const uint64_t sp = m.space & valid;
            // bit i: byte i is a blank and byte i-1 was not, or the other way round: a word ends or starts there
            uint64_t flips = (sp ^ ((sp << 1) | (in_word ? 0u : 1u))) & valid;
            while (flips) {
                const unsigned i = static_cast<unsigned>(__builtin_ctzll(flips));
                flips &= flips - 1;
                if ((sp >> i) & 1) {
                    word(start, off + i);
                    in_word = false;
                } else {
                    start = off + i;
                    in_word = true;
                }
            }
        }
        if (in_word) word(start, n);
    }

    std::string cur, run;   // cur: the part (camelCase split); run: the whole alphanumeric run it sits in
    bool split_in_run = false;
    auto emit = [&](std::string_view tok, bool digits) {
        const bool keep = digits ? tok.size() >= 3 : tok.size() >= 2;
        if (keep && (keep_stop || !is_stopword(tok))) out.push_back(stem(tok));
    };
    auto emit_str = [&](const std::string& tok) { emit(tok, std::all_of(tok.begin(), tok.end(), is_digit)); };
    auto flush = [&] {
        if (!cur.empty()) emit_str(cur);
        cur.clear();
    };
    auto end_run = [&] {
        flush();
        if (split_in_run && run.size() >= 3) emit_str(run);   // BTQuant is also btquant
        run.clear();
        split_in_run = false;
    };

    size_t i = 0;
    char prev = 0;   // the last letter's case: 'A' upper, 'a' lower or other, 0 at a break
    while (i < n) {
        // at a boundary an ASCII word begins: when it has no camelCase seam and ends at an ASCII byte (or the
        // end), it is one term, taken straight from the text with no per-character work
        if (cur.empty() && is_alnum(text[i])) {
            simd::Masks32 m;
            simd::classify32(text.data() + i, std::min<size_t>(32, n - i), &m);
            const unsigned len = m.alnum == ~uint32_t{0} ? 32u : static_cast<unsigned>(__builtin_ctz(~m.alnum));
            if (len < 32 && (i + len == n || !((m.high >> len) & 1))) {
                const uint32_t bits = (uint32_t{1} << len) - 1;
                const uint32_t up = m.upper & bits, lo = m.lower & bits;
                const uint32_t seams = (up & ((~up & bits) << 1)) | (up & (up << 1) & (lo >> 1));   // readFile, HTTPServer
                if (!seams) {
                    char buf[32];
                    std::string_view tok = text.substr(i, len);
                    if (up) {
                        for (unsigned k = 0; k < len; ++k) buf[k] = static_cast<char>(text[i + k] | (((up >> k) & 1u) << 5));
                        tok = std::string_view(buf, len);
                    }
                    emit(tok, (up | lo) == 0);
                    i += len;
                    continue;
                }
            }
        }
        const size_t at = i;
        const uint32_t cp = next_cp(text, i);
        if (cp < 0x80) {
            const char c = static_cast<char>(cp);
            if (!is_alnum(c)) {
                end_run();
                prev = 0;
                continue;
            }
            // camelCase: readFile -> read file, HTTPServer -> http server
            if (is_upper(c) && !cur.empty()) {
                const bool next_lower = i < n && is_lower(text[i]);
                if (prev == 'a' || (prev == 'A' && next_lower)) {
                    flush();
                    split_in_run = true;
                }
            }
            const char l = lower_of(c);
            cur.push_back(l);
            run.push_back(l);
            prev = is_upper(c) ? 'A' : 'a';
        } else if (symbol_cp(cp)) {
            end_run();
            prev = 0;
        } else {
            const char* f = fold_cp(cp);
            const bool upper = cp >= 0xC0 && cp <= 0xDE;   // Ä Ö Ü É ...: the case of what follows is judged from it
            if (f) {
                cur += f;
                run += f;
            } else {
                cur.append(text.substr(at, i - at));   // another script: as it is
                run.append(text.substr(at, i - at));
            }
            prev = upper ? 'A' : 'a';
        }
    }
    end_run();
    return out;
}

// ---- Distiller ----

namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A line of terminal output, a prompt, a log or a table: pasted, not written.
bool terminal_line(const std::string& line) {
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    const std::string t = line.substr(b);
    static const char* glyphs[] = {"\xE2\x95", "\xE2\x94", "\xE2\x9D\xAF", "\xCE\xBB ", "$ ", "> ", "\xE2\x9C", "\xE2\x9E", "\xE2\x87"};
    for (const char* g : glyphs)
        if (starts_with(t, g)) return true;
    if (starts_with(t, "[20") || (starts_with(t, "20") && t.size() > 10 && t[4] == '-' && t[7] == '-')) return true;
    if (starts_with(t, "{\"") || starts_with(t, "HTTP/") || (starts_with(t, "at ") && t.find('(') != std::string::npos)) return true;
    size_t alpha = 0, space = 0, other = 0;
    for (char c : t) {
        if (is_alnum(c) || (static_cast<unsigned char>(c) >= 0x80)) ++alpha;
        else if (c == ' ') ++space;
        else ++other;
    }
    return alpha + space > 0 && static_cast<double>(other) / static_cast<double>(alpha + space + other) > 0.30;
}

std::string drop_between(std::string s, const std::string& open, const std::string& close) {
    for (size_t b; (b = s.find(open)) != std::string::npos;) {
        size_t e = s.find(close, b + open.size());
        if (e == std::string::npos) e = s.size();
        else e += close.size();
        s.erase(b, e - b);
    }
    return s;
}

std::string squash(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            sp = !o.empty();
            continue;
        }
        if (sp) o.push_back(' ');
        sp = false;
        o.push_back(c);
    }
    return o;
}

}  // namespace

std::string utf8_head(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}
std::string utf8_tail(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t from = s.size() - n;
    while (from < s.size() && (static_cast<unsigned char>(s[from]) & 0xC0) == 0x80) ++from;
    return s.substr(from);
}

namespace {

// "http://x/y" -> ""
std::string without_urls(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0) {
            while (i < s.size() && s[i] != ' ' && s[i] != '\n' && s[i] != '\t') ++i;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

const std::string& user_name() {
    static const std::string u = [] {
        const char* v = std::getenv("USER");
        return v ? lower_ascii(v) : std::string();
    }();
    return u;
}

// The words a path is about: its project and file, never the generic folders on the way.
void path_terms(const std::string& word, std::vector<Term>& out, float scale = 1.0f) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : word) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    for (size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        std::string p = parts[i];
        if (p == "~" || p == "." || p == "..") continue;
        if (!last && p.front() == '.') continue;   // .config, .local, .claude on the way
        const std::string lp = lower_ascii(p);
        if (is_generic_path_word(lp) || (!user_name().empty() && lp == user_name())) continue;
        float w = last ? 1.0f : 1.3f;   // a project folder names the topic more than a file does
        if (last) {
            if (const size_t dot = p.rfind('.'); dot != std::string::npos && dot > 0 && p.size() - dot <= 6) {
                for (const auto& t : terms_of(p.substr(dot + 1))) out.push_back({t, 0.8f * scale});
                p.resize(dot);
            }
        }
        for (const auto& t : terms_of(p)) out.push_back({t, w * scale});
    }
}

}  // namespace

std::string Distiller::prose(const std::string& raw, size_t max_chars) {
    std::string s = drop_between(raw, "<pasted_content", "</pasted_content>");
    s = drop_between(s, "```", "```");
    std::string keep;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(pos, e - pos);
        if (!terminal_line(line)) keep += line + "\n";
        pos = e + 1;
    }
    s = squash(without_urls(keep));
    if (s.size() > max_chars) s = utf8_head(s, max_chars * 2 / 3) + " " + utf8_tail(s, max_chars / 3);   // the ask is often at the end
    return s;
}

Query Distiller::pick(std::vector<Term> all, std::string text, const IdfFn& idf) const {
    std::map<std::string, float> merged;
    for (const auto& t : all) {
        auto it = merged.find(t.text);
        if (it == merged.end()) merged[t.text] = t.weight;
        else it->second = std::min(2.0f, it->second + 0.1f * t.weight);   // said twice: a little more
    }
    std::vector<std::pair<std::string, float>> ranked(merged.begin(), merged.end());
    auto value = [&](const std::pair<std::string, float>& p) {
        if (idf) return p.second * idf(p.first);   // an unknown term (0) cannot match anything
        return p.second * static_cast<float>(std::min<size_t>(p.first.size(), 10)) / 5.0f;
    };
    std::stable_sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) { return value(a) > value(b); });
    Query q;
    q.text = std::move(text);
    for (const auto& r : ranked) {
        if (q.terms.size() >= opt_.max_terms) break;
        if (idf && idf(r.first) <= 0.0f) continue;
        q.terms.push_back({r.first, r.second});
    }
    return q;
}

Query Distiller::text(const std::string& s, const IdfFn& idf) const {
    const std::string p = prose(s, opt_.max_chars);
    std::vector<Term> all;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == ' ') ++i;
        size_t j = i;
        while (j < p.size() && p[j] != ' ') ++j;
        if (j > i) {
            std::string w = p.substr(i, j - i);
            float boost = 1.0f;
            auto wrapped = [&](char c) { return w.size() > 2 && w.front() == c && w.back() == c; };
            if (wrapped('`') || wrapped('"') || wrapped('\'')) {   // quoted: said on purpose
                w = w.substr(1, w.size() - 2);
                boost = 1.5f;
            }
            if (w.find('/') != std::string::npos) {
                path_terms(w, all);
            } else {
                const bool ident = w.find('-') != std::string::npos || w.find('_') != std::string::npos ||
                                   std::any_of(w.begin(), w.end(), is_digit) ||
                                   (std::any_of(w.begin() + 1, w.end(), is_upper) && std::any_of(w.begin(), w.end(), is_lower));
                for (const auto& t : terms_of(w)) all.push_back({t, boost * (ident ? 1.2f : 1.0f)});
            }
        }
        i = j;
    }
    return pick(std::move(all), p, idf);
}

Query Distiller::terms(const std::vector<std::pair<std::string, float>>& pieces, const IdfFn& idf) const {
    std::vector<Term> all;
    std::string text;
    for (const auto& [piece, weight] : pieces) {
        text += (text.empty() ? "" : " ") + piece;
        if (piece.find('/') != std::string::npos) {   // a path: its project and file, not the folders on the way
            path_terms(piece, all, weight);
            continue;
        }
        for (const auto& t : terms_of(piece)) all.push_back({t, weight});
    }
    return pick(std::move(all), std::move(text), idf);
}

}  // namespace know
}  // namespace dd
