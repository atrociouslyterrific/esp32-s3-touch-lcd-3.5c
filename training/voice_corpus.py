"""Bloop's speech corpus: example lines for each feeling and event.

Each training example looks like:

    hunger:hungry energy:awake mood:happy room:tilt you event:fall <say> ow ! the floor moved ! <end>

The model learns to write the words after <say> from the tokens before it, so on
the board Bloop can describe how it feels and what just happened, in its own
(slightly silly) voice.

Lines are built from templates with slots ({food}, {friend}, {wow} ...) plus
optional openers, second sentences about how Bloop feels, and endings, so the
model sees many thousands of different lines and learns to mix the parts rather
than memorise a fixed list. Every line stays short enough for the board's speech
bubble (MAX_CHARS) and the model's 32-word window (MAX_WORDS).
"""
import random
import re

FEELINGS = {
    "hunger": ["hungry", "full"],
    "energy": ["tired", "awake"],
    "mood": ["happy", "ok", "sad"],
    "room": ["calm", "tilt", "float"],
}
FLAGS = ["dark", "you"]   # optional: lights off, you're nearby

EVENTS = [
    "idle", "fall", "shake", "eat", "overeat", "kick", "pet", "good", "no",
    "wake", "sleep", "dance", "jump", "hide", "window", "beg", "roll", "grip",
    "float", "toy",
    # Talking with Pip, the pet from the other board, while it visits
    "visit", "pip", "chat", "bye",
]

# What Pip just said, sorted into a topic on the board (see visitor.h).
PIP_TOPICS = ["food", "sleep", "play", "weather", "love", "question", "any"]

MAX_CHARS = 56   # the radio message to Pip holds 57 characters (the bubble's buffer is 64)
MAX_WORDS = 16   # the board writes at most 16 words; prompt + line + <end> fits in 32 tokens

# ---- Slot words: each {name} in a template picks one of these ----
SLOTS = {
    "food": ["snack", "cookie", "berry", "cracker", "muffin", "noodle", "apple", "pancake",
             "jelly bean", "carrot", "biscuit", "grape", "cupcake", "pretzel", "blueberry", "pie"],
    "foods": ["snacks", "cookies", "berries", "crackers", "muffins", "noodles", "pancakes",
              "jelly beans", "grapes", "cupcakes", "pretzels", "blueberries"],
    "yum": ["yum", "yummy", "nom", "mmm", "tasty", "delicious", "scrumptious", "crunchy"],
    "friend": ["friend", "buddy", "pal", "my friend", "best friend"],
    "wow": ["wow", "whoa", "ooh", "oh", "hey", "yay", "eek", "hooray", "woohoo", "gosh"],
    "ouch": ["ow", "ouch", "oof", "bonk", "owie", "yikes", "ack"],
    "nice": ["nice", "fun", "cozy", "great", "lovely", "super", "amazing", "perfect", "awesome",
             "wonderful", "silly", "fantastic"],
    "feel_good": ["happy", "cozy", "bouncy", "great", "proud", "lucky", "warm", "giggly", "sparkly"],
    "feel_bad": ["sad", "grumpy", "lonely", "droopy", "gloomy", "blue", "wobbly", "small"],
    "spot": ["my room", "my bed", "the corner", "the floor", "the wall", "the window", "my toy box",
             "the ceiling", "my little home"],
    "move": ["bounce", "wiggle", "spin", "hop", "roll", "slide", "jiggle", "zoom", "twirl", "squish"],
    "moving": ["bouncing", "wiggling", "spinning", "hopping", "rolling", "sliding", "zooming", "twirling"],
    "body": ["tummy", "head", "nose", "belly", "chin", "bottom", "blob"],   # singular: "my {body} is"
    "sky": ["clouds", "stars", "the moon", "the sun", "a bird", "a rainbow", "rain", "snow", "a kite",
            "a plane", "the sky"],
    "toy": ["ball", "toy", "squeaky toy", "bouncy ball", "teddy"],
    "dream": ["snacks", "flying", "pip", "a giant cookie", "the stars", "bouncing", "clouds", "my friends"],
    "when": ["today", "right now", "all day", "again", "later", "soon", "every day"],
    "very": ["so", "very", "super", "really", "extra", "a little"],
    "sound": ["la la la", "doo doo doo", "hum hum", "bloop bloop", "tra la la", "beep boop"],
    "num": ["two", "three", "five", "ten", "a hundred", "a million"],
    "color": ["red", "blue", "green", "pink", "purple", "yellow", "orange"],
    "thing": ["the window", "my bed", "a snack", "the wall", "my toy", "the stars", "pip", "a puzzle",
              "the floor", "a song"],
}

# ---- What Bloop says about the event ----
EVENT_LINES = {
    "idle": [
        "bloop .", "hmm hmm .", "what should bloop do ?", "{sound} .", "i like {spot} .",
        "is it snack time yet ?", "bloop bloop .", "just thinking .", "this is a {nice} wall .",
        "i wonder what is outside .", "my {body} feels wiggly .", "tap me if you are there !",
        "i am thinking about {thing} .", "what a {nice} day .", "i could {move} {when} .",
        "i wonder if pip is awake .", "do blobs have dreams ?", "what is behind the wall ?",
        "i counted {num} dots on the wall .", "my favourite color is {color} .",
        "i wish i had a {food} .", "maybe i will {move} a bit .", "just a blob being a blob .",
        "the {spot} is very quiet .", "is anybody there ?", "i am bored , let us play !",
        "today i will be brave .", "i learned to {move} !", "{wow} , i had an idea !",
        "{sound} , bloop is humming .", "nothing to do but {move} .", "i like it here .",
        "hello , is it you ?", "i should tidy {spot} .", "i feel {feel_good} {when} .",
    ],
    "fall": [
        "{ouch} !", "{ouch} , {ouch} !", "the floor moved !", "who tipped the room ?",
        "that hurt .", "my {body} !", "i fell down .", "the wall came out of nowhere !",
        "please hold the room still !", "{ouch} , that was a big fall .", "who moved the floor ?",
        "i bumped my {body} .", "down i go !", "not again !", "i need a bandage .",
        "the room flipped over !", "{ouch} , i landed on my {body} .", "gravity is not fair .",
        "careful with the room , please !", "i saw stars , and not the sky ones .",
        "{ouch} ! i was not ready !", "that floor is very hard .",
    ],
    "shake": [
        "whoa !", "earthquake !", "stop shaking !", "everything is spinning !", "my brain is wobbly !",
        "too much shaking !", "aaah !", "the room is going crazy !", "i am all jiggly !",
        "my {body} is shaking !", "please stop , i feel dizzy .", "shaky shaky !",
        "{wow} , the whole room is wobbling !", "i am a blob smoothie now .", "is this a storm ?",
        "hold the room still , please !", "the walls are dancing !", "i feel like jelly .",
    ],
    "eat": [
        "{yum} !", "nom nom nom .", "{yum} , a {food} !", "that was a {nice} snack .",
        "crunch crunch .", "more please !", "thank you for the food !", "my tummy is happy .",
        "i love {foods} !", "best {food} ever !", "{yum} , {yum} !", "this {food} is {yum} .",
        "a {food} ! my favourite !", "munch munch .", "i could eat {num} {foods} .",
        "{food} for bloop ? thank you !", "slurp !", "that hit the spot .",
        "{yum} , i was so hungry .", "my tummy says thank you .",
    ],
    "overeat": [
        "too much food .", "my tummy hurts .", "i was not even hungry .", "ugh , so full .",
        "no more snacks please .", "bloop ate too much .", "i think i will pop .",
        "not one more {food} .", "i am a round blob now .", "ugh , too many {foods} .",
        "my tummy is too full to {move} .", "maybe i should not have eaten that .",
    ],
    "kick": [
        "kick !", "got it !", "goal !", "wheee , go {toy} !", "boing !", "catch it !",
        "i am so fast !", "again , again !", "watch my {toy} fly !", "bonk , got the {toy} !",
        "{wow} , what a kick !", "i am the {toy} champion !", "go , go , go !",
        "kick kick kick !", "did you see that kick ?", "the {toy} goes zoom !",
    ],
    "pet": [
        "hehe !", "that tickles !", "more pats please .", "i love pats .", "purr purr .",
        "you are my favourite .", "so cozy .", "hi {friend} !", "pat pat pat .",
        "your hand is so warm .", "i feel {feel_good} .", "right on my {body} , perfect !",
        "hehe , my {body} is ticklish .", "do not stop !", "i could get pats all day .",
        "you are a {nice} {friend} .", "squish squish , i like that .", "aww , thank you .",
    ],
    "good": [
        "yay !", "i did it !", "bloop is a good bloop !", "thank you !", "i will do that again !",
        "hooray !", "best day ever !", "i feel so {feel_good} !", "{wow} , really ?",
        "i am a {nice} blob !", "you think i did good ?", "i knew i could do it !",
        "this is the best !", "i am so proud .", "thank you , {friend} !", "good bloop , yes !",
    ],
    "no": [
        "sorry .", "oops .", "okay , okay .", "i will not do that again .", "aww .",
        "bloop is sad now .", "but i wanted to !", "i am sorry , {friend} .", "i did not mean it .",
        "i will be good , i promise .", "oh no , i messed up .", "hmph , fine .", "i feel {feel_bad} now .",
        "can we still be friends ?", "i will try better .", "my {body} droops .",
    ],
    "wake": [
        "good morning !", "*yawn* .", "i had a {nice} nap .", "what did i miss ?", "awake now !",
        "five more minutes ...", "i dreamed about {dream} .", "rise and shine !",
        "{wow} , i slept so long .", "my {body} is still sleepy .", "is it morning ?",
        "time to {move} !", "i feel {feel_good} after that nap .", "hello , new day !",
        "*stretch* , good morning .",
    ],
    "sleep": [
        "nap time .", "good night .", "so sleepy ...", "zzz ...", "time for bed .",
        "my bed is the best .", "night night .", "i will dream about {dream} .",
        "sweet dreams , {friend} .", "my bed is so {nice} .", "*yawn* , bedtime .",
        "do not wake me , okay ?", "goodnight , room .", "snuggle time .", "i am off to dreamland .",
    ],
    "dance": [
        "dance party !", "{sound} !", "look at my moves !", "wiggle wiggle !", "boogie time !",
        "i got the rhythm !", "spin and spin !", "watch me {move} !", "{move} , {move} , {move} !",
        "this is my {nice} dance .", "i am a dancing blob !", "dance with me !",
        "left , right , {move} !", "the floor is my stage !", "i invented a new dance !",
    ],
    "jump": [
        "boing !", "jump jump !", "look how high !", "i can almost touch the top !", "hop hop !",
        "wheee !", "up , up , up !", "{wow} , so high !", "i am a bouncy blob !",
        "i can jump {num} times !", "higher , higher !", "boing , boing , boing !",
        "watch me fly !", "i touched the ceiling , almost !",
    ],
    "hide": [
        "hiding !", "you cannot see me .", "safe by my bed .", "eep !", "shh , i am hiding .",
        "is it safe yet ?", "the bed will protect me .", "i am not here .", "shh , very quiet .",
        "no one can find me in {spot} .", "peek a boo !", "i will hide until it stops .",
        "{wow} , that was scary .", "hiding is a {nice} plan .",
    ],
    "window": [
        "ooh , clouds .", "i see the sky .", "what is out there ?", "the window is pretty .",
        "i wish i could fly .", "hello , outside !", "so many stars .", "i see {sky} !",
        "{wow} , look at {sky} .", "is pip out there somewhere ?", "the world is so big .",
        "i wonder where {sky} goes .", "one day i will go outside .", "{sky} looks {nice} today .",
        "i could watch {sky} all day .",
    ],
    "beg": [
        "food please ?", "can i have a snack ?", "i am so hungry .", "please , please , please ?",
        "my tummy is talking .", "snack ? snack ? snack ?", "feed the bloop !",
        "maybe a {food} ?", "i would love a {food} .", "just one {food} , please ?",
        "my tummy is rumbling for {foods} .", "is there any {food} for me ?",
        "i will be good for a {food} .", "hungry blob here !",
    ],
    "roll": [
        "roly poly !", "wheee , rolling !", "i am a ball now !", "round and round !",
        "rolling is fun !", "tumble tumble !", "i am {moving} !", "roll , roll , roll !",
        "look , i am a wheel !", "{wow} , the room is spinning !", "rolling to {spot} !",
    ],
    "grip": [
        "hold on !", "i am holding the wall !", "do not let go !", "grip grip grip !",
        "the room is tilting , hold tight !", "i will not fall this time !",
        "hang on , bloop !", "my {body} is holding on !", "sticky blob power !",
        "{wow} , the floor is a hill !", "i am a strong blob !", "not falling , not today !",
    ],
    "float": [
        "i am floating !", "zero gravity !", "wheee , space !", "no up , no down !",
        "i can fly !", "where is the floor ?", "i am a space blob !", "look , i am {moving} in the air !",
        "{wow} , i weigh nothing !", "floaty floaty !", "i am an astronaut !",
        "up here i can see {spot} .",
    ],
    "visit": [
        "pip ! you came !", "hi pip !", "welcome to my room , pip !", "yay , pip is here !",
        "pip , come in !", "my friend pip is here !", "{wow} , pip ! hello !",
        "pip ! i missed you !", "come see my room , pip !", "hello , {friend} pip !",
        "pip is here , {when} is the best !", "make yourself at home , pip .",
    ],
    "chat": [
        "pip , look at me !", "pip , want to play ?", "pip , what do you do all day ?", "hey pip !",
        "pip , my room tilts sometimes .", "pip , do you like my bed ?", "pip , guess what ?",
        "pip , i learned a new trick !", "pip , do you like {foods} ?", "pip , i can {move} !",
        "pip , what is your favourite color ?", "pip , i saw {sky} today .",
        "pip , want to see me {move} ?", "pip , let us {move} together !", "pip , are you hungry ?",
        "pip , i dreamed about {dream} .", "pip , my favourite color is {color} .",
    ],
    "bye": [
        "bye , pip !", "come back soon , pip !", "see you later , pip .", "aww , pip left .",
        "bye bye , {friend} !", "visit again , pip !", "i will miss you , pip .",
        "thanks for coming , pip !", "bye pip , sleep well .", "see you {when} , pip !",
    ],
    "toy": [
        "my {toy} !", "toy time !", "come here , {toy} !", "i love my {toy} .", "play with me !",
        "{wow} , my {toy} !", "the {toy} is {nice} .", "i will {move} with my {toy} .",
        "best {toy} ever !", "catch , {toy} !",
    ],
}

# ---- Bloop answering Pip, by what Pip talked about ----
PIP_REPLIES = {
    "food": ["snacks are the best , pip !", "i am hungry too !", "let us eat together !",
             "you like food like me !", "yum , food talk !", "i love {foods} , pip !",
             "want to share a {food} , pip ?", "{yum} , pip , {foods} !", "a {food} sounds {nice} !"],
    "sleep": ["naps are cozy .", "you can sleep in my bed , pip .", "shh , pip is sleepy .",
              "i like naps too .", "my bed is soft , pip .", "sweet dreams , pip .",
              "i dream about {dream} , pip .", "let us nap together , pip ."],
    "play": ["let us play , pip !", "dance with me , pip !", "you sing so well , pip !",
             "play time !", "you are fun , pip !", "let us {move} , pip !", "race you , pip !",
             "watch me {move} , pip !", "tag , pip , you are it !"],
    "weather": ["it is always nice in my room .", "brr , is it cold , pip ?", "i never see the sun in here .",
                "is it warm where you live ?", "i see {sky} from my window .",
                "is there {sky} where you are , pip ?", "my room has no weather , pip ."],
    "love": ["you are my best friend , pip !", "i love you too , pip !", "hug !", "pip is the best .",
             "you make me feel {feel_good} , pip .", "best friends forever , pip !",
             "aww , pip , you are {nice} ."],
    "question": ["hmm , i do not know , pip .", "yes , pip !", "maybe ?", "good question , pip !",
                 "let me think ...", "i think so , pip .", "hmm , ask me later , pip .",
                 "nope , pip !", "what do you think , pip ?"],
    "any": ["really , pip ?", "me too , pip !", "haha , pip is funny .", "tell me more , pip .",
            "wow !", "okay , pip !", "{wow} , pip !", "you are so {nice} , pip .",
            "i did not know that , pip !", "hehe , pip !"],
}

# ---- Extra bits about how Bloop feels (added sometimes) ----
FEELING_LINES = {
    ("hunger", "hungry"): ["i am hungry .", "my tummy is rumbling .", "snack soon ?", "so hungry .",
                           "i could eat a {food} .", "my tummy wants {foods} ."],
    ("hunger", "full"): ["my tummy is full .", "i am not hungry ."],
    ("energy", "tired"): ["i am sleepy .", "so tired .", "*yawn* .", "need a nap .", "my {body} is sleepy ."],
    ("energy", "awake"): ["i am wide awake !", "i have so much energy !"],
    ("mood", "happy"): ["life is good !", "i am happy !", "today is great !", "i feel {feel_good} !",
                        "i am a {nice} blob today !"],
    ("mood", "sad"): ["i feel sad .", "nobody plays with me .", "sniff .", "i feel {feel_bad} .",
                      "i need a hug ."],
    ("room", "tilt"): ["the room is tilting !", "why is the floor sideways ?", "hold the room still !",
                       "everything is sliding !"],
    ("room", "float"): ["i am floating !", "everything is floaty ."],
    ("flag", "dark"): ["it is dark in here .", "who turned off the lights ?", "so dark .",
                       "i can not see {spot} ."],
    ("flag", "you"): ["hi {friend} !", "i see you !", "you are here !", "hello , you !"],
}

# Optional bits at the start or end of a line.
OPENERS = ["{wow} ,", "hey ,", "oh ,", "hmm ,", "well ,", "hehe ,", "psst ,", "guess what ,"]
ENDINGS = ["{friend} !", "hehe !", "{wow} !", "bloop !", "yes !"]
CONNECT = ["", "", "and", "also", "but", "so", "plus"]

# Lines that already start with an exclamation don't get another opener ("hmm , hooray").
EXCLAIMS = set(SLOTS["wow"] + SLOTS["ouch"] + SLOTS["yum"]) | {"pip", "hi", "yay", "hehe", "boing", "wheee",
                                                               "whoa", "kick", "goal", "zzz", "hooray", "bloop"}
SLOT_RE = re.compile(r"\{(\w+)\}")
NO_OPENER = {"pip", "visit", "bye", "fall", "shake"}   # these start straight away


def fill(template, rng):
    return SLOT_RE.sub(lambda m: rng.choice(SLOTS[m.group(1)]), template).split()


def sample_feelings(rng):
    f = {k: rng.choice(v) for k, v in FEELINGS.items()}
    flags = [x for x in FLAGS if rng.random() < 0.3]
    return f, flags


def prompt_tokens(f, flags, event, topic=None):
    toks = [f"hunger:{f['hunger']}", f"energy:{f['energy']}", f"mood:{f['mood']}", f"room:{f['room']}"]
    toks += flags
    if event == "pip":
        toks.append(f"pip:{topic or 'any'}")
    toks += [f"event:{event}", "<say>"]
    return toks


def line_text(words):
    s = ""
    for w in words:
        if s and w not in (".", "!", "?", ",", "..."):
            s += " "
        s += w
    return s


def _make_line(rng, f, flags, event, topic=None):
    if event == "pip":
        words = fill(rng.choice(PIP_REPLIES[topic]), rng)   # answer Pip, don't change the subject
        if rng.random() < 0.15 and words[0] not in EXCLAIMS:
            words = fill(rng.choice(OPENERS), rng) + words
        return words
    words = fill(rng.choice(EVENT_LINES[event]), rng)
    if event not in NO_OPENER and rng.random() < 0.15 and words[0] not in EXCLAIMS:
        words = fill(rng.choice(OPENERS), rng) + words
    # Sometimes add a second part about how Bloop feels, but only true feelings.
    options = [(k, v) for k, v in f.items() if (k, v) in FEELING_LINES]
    options += [("flag", x) for x in flags if ("flag", x) in FEELING_LINES]
    # Don't repeat the event's own topic.
    if event == "float":
        options = [o for o in options if o != ("room", "float")]
    if event in ("beg", "eat"):
        options = [o for o in options if o[0] != "hunger"]
    if event == "overeat":
        options = [o for o in options if o != ("hunger", "full")]
    chance = 0.8 if event == "chat" else 0.5   # chatting: tell Pip how you are
    if options and rng.random() < chance:
        extra = fill(rng.choice(FEELING_LINES[rng.choice(options)]), rng)
        c = rng.choice(CONNECT)
        full_sentence = extra[0] in ("i", "my", "the", "it", "life", "today", "nobody", "who", "why",
                                     "you", "everything", "hello")
        words += ([c] if c and full_sentence else []) + extra
    elif rng.random() < 0.1 and words[-1] in ("!", "."):
        words += fill(rng.choice(ENDINGS), rng)
    return words


def make_line(rng, f, flags, event, topic=None):
    # Keep lines short enough for the board; try again if one comes out too long.
    for _ in range(20):
        words = _make_line(rng, f, flags, event, topic)
        if len(words) <= MAX_WORDS and len(line_text(words)) <= MAX_CHARS:
            return words
    return words[:MAX_WORDS]


def make_example(rng):
    f, flags = sample_feelings(rng)
    event = rng.choice(EVENTS)
    # Keep events consistent with feelings most of the time.
    if event == "beg" and rng.random() < 0.8:
        f["hunger"] = "hungry"
    if event == "overeat":
        f["hunger"] = "full"
    if event == "sleep" and rng.random() < 0.8:
        f["energy"] = "tired"
    if event == "wake":
        f["energy"] = "awake"
    if event == "float":
        f["room"] = "float"
    if event == "grip" and rng.random() < 0.8:
        f["room"] = "tilt"
    if event == "pet":
        if "you" not in flags:
            flags.append("you")
    topic = rng.choice(PIP_TOPICS) if event == "pip" else None
    return prompt_tokens(f, flags, event, topic) + make_line(rng, f, flags, event, topic) + ["<end>"]


def build_vocab():
    words = set()
    texts = [t for lines in EVENT_LINES.values() for t in lines]
    texts += [t for lines in FEELING_LINES.values() for t in lines]
    texts += [t for lines in PIP_REPLIES.values() for t in lines]
    texts += OPENERS + ENDINGS + [c for c in CONNECT if c]
    texts += [w for opts in SLOTS.values() for w in opts]
    for t in texts:
        words.update(SLOT_RE.sub(" ", t).split())
    specials = ["<end>", "<say>"]
    prompt = [f"{k}:{v}" for k, vs in FEELINGS.items() for v in vs] + FLAGS + [f"event:{e}" for e in EVENTS]
    prompt += [f"pip:{t}" for t in PIP_TOPICS]
    return specials + prompt + sorted(words - set(specials) - set(prompt))


def corpus(n, seed=7):
    rng = random.Random(seed)
    return [make_example(rng) for _ in range(n)]


if __name__ == "__main__":
    v = build_vocab()
    c = corpus(40000)
    lines = {" ".join(ex[ex.index("<say>") + 1:-1]) for ex in c}
    print(len(v), "tokens;", len(lines), "different lines in 40,000 examples")
    print("longest:", max(len(line_text(ex[ex.index("<say>") + 1:-1])) for ex in c), "chars,",
          max(len(ex) for ex in c), "tokens")
    for ex in corpus(12, seed=1):
        print(" ".join(ex))
