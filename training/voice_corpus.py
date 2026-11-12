"""Bloop's speech corpus: example lines for each feeling and event.

Each training example looks like:

    hunger:hungry energy:awake mood:happy room:tilt you event:fall <say> ow ! the floor moved ! <end>

The model learns to write the words after <say> from the tokens before it, so on
the board Bloop can describe how it feels and what just happened, in its own
(slightly silly) voice. Lines are built from parts, so the model sees lots of
combinations and learns to mix them rather than memorise a fixed list.
"""
import random

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
]

# ---- What Bloop says about the event ----
EVENT_LINES = {
    "idle": [
        "bloop .", "hmm hmm .", "what should bloop do ?", "la la la .", "i like my room .",
        "is it snack time yet ?", "bloop bloop .", "just thinking .", "this is a nice wall .",
        "i wonder what is outside .", "my ears feel wiggly .", "tap me if you are there !",
    ],
    "fall": [
        "ow !", "ouch !", "oof !", "ow ow ow !", "the floor moved !", "who tipped the room ?",
        "that hurt .", "bonk !", "my head !", "i fell down .", "the wall came out of nowhere !",
        "please hold the room still !",
    ],
    "shake": [
        "whoa !", "earthquake !", "stop shaking !", "everything is spinning !", "my brain is wobbly !",
        "too much shaking !", "aaah !", "the room is going crazy !",
    ],
    "eat": [
        "yum !", "nom nom nom .", "tasty !", "that was a good snack .", "crunch crunch .",
        "more please !", "delicious !", "thank you for the food !", "my tummy is happy .",
    ],
    "overeat": [
        "too much food .", "my tummy hurts .", "i was not even hungry .", "ugh , so full .",
        "no more snacks please .", "bloop ate too much .",
    ],
    "kick": [
        "kick !", "got it !", "goal !", "wheee , go ball !", "boing !", "catch it !",
        "i am so fast !", "again , again !",
    ],
    "pet": [
        "hehe !", "that tickles !", "more pats please .", "i love pats .", "purr purr .",
        "you are my favourite .", "so cozy .", "hi friend !", "pat pat pat .",
    ],
    "good": [
        "yay !", "i did it !", "bloop is a good bloop !", "thank you !", "i will do that again !",
        "hooray !", "best day ever !",
    ],
    "no": [
        "sorry .", "oops .", "okay , okay .", "i will not do that again .", "aww .",
        "bloop is sad now .", "but i wanted to !",
    ],
    "wake": [
        "good morning !", "*yawn* .", "i had a nice nap .", "what did i miss ?", "awake now !",
        "five more minutes ...", "i dreamed about snacks .",
    ],
    "sleep": [
        "nap time .", "good night .", "so sleepy ...", "zzz ...", "time for bed .",
        "my bed is the best .", "night night .",
    ],
    "dance": [
        "dance party !", "la la la !", "look at my moves !", "wiggle wiggle !", "boogie time !",
        "i got the rhythm !", "spin and spin !",
    ],
    "jump": [
        "boing !", "jump jump !", "look how high !", "i can almost touch the top !", "hop hop !",
        "wheee !", "up , up , up !",
    ],
    "hide": [
        "hiding !", "you cannot see me .", "safe by my bed .", "eep !", "shh , i am hiding .",
        "is it safe yet ?", "the bed will protect me .",
    ],
    "window": [
        "ooh , clouds .", "i see the sky .", "what is out there ?", "the window is pretty .",
        "i wish i could fly .", "hello , outside !", "so many stars .",
    ],
    "beg": [
        "food please ?", "can i have a snack ?", "i am so hungry .", "please , please , please ?",
        "my tummy is talking .", "snack ? snack ? snack ?", "feed the bloop !",
    ],
    "roll": [
        "roly poly !", "wheee , rolling !", "i am a ball now !", "round and round !",
        "rolling is fun !", "tumble tumble !",
    ],
    "grip": [
        "hold on !", "i am holding the wall !", "do not let go !", "grip grip grip !",
        "the room is tilting , hold tight !", "i will not fall this time !",
    ],
    "float": [
        "i am floating !", "zero gravity !", "wheee , space !", "no up , no down !",
        "i can fly !", "where is the floor ?",
    ],
    "toy": [
        "my toy !", "toy time !", "come here , toy !", "i love my toy .", "play with me !",
    ],
}

# ---- Extra bits about how Bloop feels (added sometimes) ----
FEELING_LINES = {
    ("hunger", "hungry"): ["i am hungry .", "my tummy is rumbling .", "snack soon ?", "so hungry ."],
    ("energy", "tired"): ["i am sleepy .", "so tired .", "*yawn* .", "need a nap ."],
    ("mood", "happy"): ["life is good !", "i am happy !", "today is great !"],
    ("mood", "sad"): ["i feel sad .", "nobody plays with me .", "sniff ."],
    ("room", "tilt"): ["the room is tilting !", "why is the floor sideways ?", "hold the room still !"],
    ("room", "float"): ["i am floating !", "everything is floaty ."],
    ("flag", "dark"): ["it is dark in here .", "who turned off the lights ?", "so dark ."],
    ("flag", "you"): ["hi friend !", "i see you !", "you are here !"],
}

CONNECT = ["", "", "and", "also", "but"]


def sample_feelings(rng):
    f = {k: rng.choice(v) for k, v in FEELINGS.items()}
    flags = [x for x in FLAGS if rng.random() < 0.3]
    return f, flags


def prompt_tokens(f, flags, event):
    toks = [f"hunger:{f['hunger']}", f"energy:{f['energy']}", f"mood:{f['mood']}", f"room:{f['room']}"]
    toks += flags
    toks += [f"event:{event}", "<say>"]
    return toks


def make_line(rng, f, flags, event):
    words = rng.choice(EVENT_LINES[event]).split()
    # Sometimes add a second part about how Bloop feels, but only true feelings.
    options = [(k, v) for k, v in f.items() if (k, v) in FEELING_LINES]
    options += [("flag", x) for x in flags if ("flag", x) in FEELING_LINES]
    # Don't repeat the event's own topic.
    if event in ("float",):
        options = [o for o in options if o != ("room", "float")]
    if event == "beg":
        options = [o for o in options if o != ("hunger", "hungry")]
    if options and rng.random() < 0.55:
        k = rng.choice(options)
        extra = rng.choice(FEELING_LINES[k]).split()
        c = rng.choice(CONNECT)
        full_sentence = extra[0] in ("i", "my", "the", "it", "life", "today", "nobody", "who", "why", "you", "everything")
        if c and full_sentence:
            words += [c] + extra
        else:
            words += extra
    return words


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
    if event == "float":
        f["room"] = "float"
    if event == "grip" and rng.random() < 0.8:
        f["room"] = "tilt"
    if event == "pet":
        if "you" not in flags:
            flags.append("you")
    return prompt_tokens(f, flags, event) + make_line(rng, f, flags, event) + ["<end>"]


def build_vocab():
    words = set()
    for lines in list(EVENT_LINES.values()) + list(FEELING_LINES.values()):
        for line in lines:
            words.update(line.split())
    words.update(c for c in CONNECT if c)
    specials = ["<end>", "<say>"]
    prompt = [f"{k}:{v}" for k, vs in FEELINGS.items() for v in vs] + FLAGS + [f"event:{e}" for e in EVENTS]
    return specials + prompt + sorted(words)


def corpus(n, seed=7):
    rng = random.Random(seed)
    return [make_example(rng) for _ in range(n)]


if __name__ == "__main__":
    v = build_vocab()
    print(len(v), "tokens")
    for ex in corpus(8):
        print(" ".join(ex))
