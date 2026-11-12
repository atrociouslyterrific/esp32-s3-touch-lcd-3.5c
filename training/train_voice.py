"""Train Bloop's voice: a tiny GPT-style transformer language model.

Word-level, decoder-only transformer (2 layers, 4 attention heads, 64-wide).
It reads Bloop's feelings + what just happened, then writes a short line.
Runs on GitHub Actions. Writes:
  bloop_room/voice_weights.h   - 8-bit weights + vocabulary for the board
  training/out/voice_test.txt  - greedy outputs the C code must reproduce
  training/out/voice_samples.json - example lines + training stats (for the website)
"""
import json
import math
import os
import sys
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_corpus as VC  # noqa: E402


# ---------- live progress for the website (GitHub's workflow pushes this file to the "status" branch) ----------
STATUS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "training", "out", "status.json")
_status = {}


def write_status(**kw):
    _status.update(kw)
    _status["updated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    os.makedirs(os.path.dirname(STATUS), exist_ok=True)
    tmp = STATUS + ".tmp"
    with open(tmp, "w") as f:
        json.dump(_status, f)
    os.replace(tmp, STATUS)  # never leave a half-written file for the uploader

SEED = 3
D, H, L, T = 64, 4, 2, 32          # width, heads, layers, context length
FF = 4 * D
STEPS = int(os.environ.get("BLOOP_VOICE_STEPS", "3000"))
BATCH, LR = 96, 3e-3

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "training", "out")
HEADER = os.path.join(ROOT, "bloop_room", "voice_weights.h")

VOCAB = VC.build_vocab()
IDX = {w: i for i, w in enumerate(VOCAB)}
V = len(VOCAB)
END, SAY = IDX["<end>"], IDX["<say>"]


class Block(nn.Module):
    def __init__(self):
        super().__init__()
        self.ln1 = nn.LayerNorm(D)
        self.qkv = nn.Linear(D, 3 * D)
        self.o = nn.Linear(D, D)
        self.ln2 = nn.LayerNorm(D)
        self.f1 = nn.Linear(D, FF)
        self.f2 = nn.Linear(FF, D)

    def forward(self, x):
        B, N, _ = x.shape
        q, k, v = self.qkv(self.ln1(x)).split(D, dim=2)
        hd = D // H
        q = q.view(B, N, H, hd).transpose(1, 2)
        k = k.view(B, N, H, hd).transpose(1, 2)
        v = v.view(B, N, H, hd).transpose(1, 2)
        att = (q @ k.transpose(-2, -1)) / math.sqrt(hd)
        mask = torch.triu(torch.ones(N, N, dtype=torch.bool), 1)
        att = att.masked_fill(mask, float("-inf")).softmax(-1)
        y = (att @ v).transpose(1, 2).reshape(B, N, D)
        x = x + self.o(y)
        x = x + self.f2(F.gelu(self.f1(self.ln2(x)), approximate="tanh"))
        return x


class TinyGPT(nn.Module):
    def __init__(self):
        super().__init__()
        self.tok = nn.Embedding(V, D)
        self.pos = nn.Embedding(T, D)
        self.blocks = nn.ModuleList([Block() for _ in range(L)])
        self.lnf = nn.LayerNorm(D)

    def forward(self, idx):
        N = idx.shape[1]
        x = self.tok(idx) + self.pos(torch.arange(N))
        for b in self.blocks:
            x = b(x)
        return self.lnf(x) @ self.tok.weight.t()   # output tied to the input embedding


def encode(ex):
    return [IDX[w] for w in ex]


def make_batch(rng_examples, rng):
    xs, ys = [], []
    for _ in range(BATCH):
        ids = encode(rng_examples[rng.integers(len(rng_examples))])[:T + 1]
        say = ids.index(SAY)
        x = ids[:-1] + [END] * (T - len(ids) + 1)
        y = ids[1:] + [-1] * (T - len(ids) + 1)
        # Only learn the words Bloop says, not the prompt.
        y = [-1 if i < say else t for i, t in enumerate(y)]
        xs.append(x[:T]); ys.append(y[:T])
    return torch.tensor(xs), torch.tensor(ys)


# ---------- 8-bit export, and a numpy model that does exactly what the C code does ----------

def quant_rows(w):
    w = np.asarray(w, dtype=np.float32)
    s = np.abs(w).max(axis=1) / 127.0
    s[s == 0] = 1e-8
    q = np.clip(np.round(w / s[:, None]), -127, 127).astype(np.int8)
    return q, s.astype(np.float32)


def export_params(model):
    sd = {k: v.detach().numpy().astype(np.float32) for k, v in model.state_dict().items()}
    P = {"tok": quant_rows(sd["tok.weight"]), "pos": sd["pos.weight"], "layers": []}
    for i in range(L):
        p = f"blocks.{i}."
        P["layers"].append({
            "ln1": (sd[p + "ln1.weight"], sd[p + "ln1.bias"]),
            "qkv": quant_rows(sd[p + "qkv.weight"]) + (sd[p + "qkv.bias"],),
            "o": quant_rows(sd[p + "o.weight"]) + (sd[p + "o.bias"],),
            "ln2": (sd[p + "ln2.weight"], sd[p + "ln2.bias"]),
            "f1": quant_rows(sd[p + "f1.weight"]) + (sd[p + "f1.bias"],),
            "f2": quant_rows(sd[p + "f2.weight"]) + (sd[p + "f2.bias"],),
        })
    P["lnf"] = (sd["lnf.weight"], sd["lnf.bias"])
    return P


def np_ln(x, g, b):
    m = x.mean()
    var = ((x - m) ** 2).mean()
    return (x - m) / np.sqrt(var + 1e-5) * g + b


def np_lin(x, qsb):
    q, s, b = qsb
    return (q.astype(np.float32) @ x) * s + b


def np_gelu(x):
    return 0.5 * x * (1.0 + np.tanh(0.7978845608 * (x + 0.044715 * x ** 3)))


class NpVoice:
    """Step-by-step model with a key/value cache: the same maths as voice.h."""

    def __init__(self, P):
        self.P = P
        self.k = np.zeros((L, T, D), np.float32)
        self.v = np.zeros((L, T, D), np.float32)
        self.pos = 0

    def step(self, tok):
        P = self.P
        tq, ts = P["tok"]
        x = tq[tok].astype(np.float32) * ts[tok] + P["pos"][self.pos]
        hd = D // H
        for li, lay in enumerate(P["layers"]):
            h = np_ln(x, *lay["ln1"])
            qkv = np_lin(h, lay["qkv"])
            q, k, v = qkv[:D], qkv[D:2 * D], qkv[2 * D:]
            self.k[li, self.pos] = k
            self.v[li, self.pos] = v
            out = np.zeros(D, np.float32)
            for hh in range(H):
                sl = slice(hh * hd, (hh + 1) * hd)
                sc = self.k[li, :self.pos + 1, sl] @ q[sl] / math.sqrt(hd)
                p = np.exp(sc - sc.max()); p /= p.sum()
                out[sl] = p @ self.v[li, :self.pos + 1, sl]
            x = x + np_lin(out, lay["o"])
            h = np_ln(x, *lay["ln2"])
            x = x + np_lin(np_gelu(np_lin(h, lay["f1"])), lay["f2"])
        h = np_ln(x, *P["lnf"])
        self.pos += 1
        return (tq.astype(np.float32) @ h) * ts


def np_generate(P, prompt, max_new=16, rng=None, temp=0.0, topk=12):
    m = NpVoice(P)
    logits = None
    for t in prompt:
        logits = m.step(t)
    out = []
    while len(out) < max_new and m.pos < T:
        if temp <= 0:
            nxt = int(np.argmax(logits))
        else:
            top = np.argsort(-logits)[:topk]
            z = logits[top] / temp
            p = np.exp(z - z.max()); p /= p.sum()
            nxt = int(top[rng.choice(len(top), p=p)])
        if nxt == END:
            break
        out.append(nxt)
        logits = m.step(nxt)
    return out


def detok(ids):
    s = ""
    for i in ids:
        w = VOCAB[i]
        if s and w not in (".", "!", "?", ",", "..."):
            s += " "
        s += w
    return s


# ---------- C header ----------

def c_arr(name, arr, ctype, per_line=24):
    flat = np.asarray(arr).ravel()
    if ctype == "int8_t":
        vals = [str(int(v)) for v in flat]
    else:
        vals = [repr(float(np.float32(v))) + "f" for v in flat]
    lines = [", ".join(vals[i:i + per_line]) for i in range(0, len(vals), per_line)]
    return f"static const {ctype} {name}[{len(flat)}] = {{\n  " + ",\n  ".join(lines) + "\n};\n"


def write_header(P, stats):
    parts = [
        "// Bloop's voice: weights of a tiny transformer language model.\n",
        "// GENERATED by training/train_voice.py on GitHub. Do not edit.\n",
        "#pragma once\n#include <stdint.h>\n\n",
        f"#define BV_V {V}\n#define BV_D {D}\n#define BV_H {H}\n#define BV_L {L}\n#define BV_T {T}\n#define BV_FF {FF}\n",
        f"#define BV_TRAIN_STEPS {stats['steps']}\n#define BV_TRAIN_LOSS {stats['loss']:.4f}f\n",
        f"#define BV_TOK_END {END}\n#define BV_TOK_SAY {SAY}\n",
    ]
    for w in VOCAB:
        if ":" in w or w in VC.FLAGS:
            name = w.replace(":", "_").upper()
            parts.append(f"#define BV_TOK_{name} {IDX[w]}\n")
    parts.append("\nstatic const char *const BV_WORDS[BV_V] = {\n  ")
    parts.append(", ".join(json.dumps(w) for w in VOCAB))
    parts.append("\n};\n\n")
    parts.append(c_arr("BV_TOK_Q", P["tok"][0], "int8_t"))
    parts.append(c_arr("BV_TOK_S", P["tok"][1], "float"))
    parts.append(c_arr("BV_POS", P["pos"], "float"))

    def stack(key, idx):
        return np.stack([lay[key][idx] for lay in P["layers"]])
    for key in ("ln1", "ln2"):
        parts.append(c_arr(f"BV_{key.upper()}_G", stack(key, 0), "float"))
        parts.append(c_arr(f"BV_{key.upper()}_B", stack(key, 1), "float"))
    for key in ("qkv", "o", "f1", "f2"):
        parts.append(c_arr(f"BV_{key.upper()}_Q", stack(key, 0), "int8_t"))
        parts.append(c_arr(f"BV_{key.upper()}_S", stack(key, 1), "float"))
        parts.append(c_arr(f"BV_{key.upper()}_B", stack(key, 2), "float"))
    parts.append(c_arr("BV_LNF_G", P["lnf"][0], "float"))
    parts.append(c_arr("BV_LNF_B", P["lnf"][1], "float"))
    with open(HEADER, "w") as f:
        f.write("".join(parts))


CKPT_IN = os.path.join(ROOT, "training", "ckpt", "voice.pt")    # last kept model (downloaded by the workflow)
CKPT_OUT = os.path.join(OUT, "ckpt", "voice.pt")                 # model to save for the next run


def make_eval_batches():
    """A fixed set of lines the model never trains on: the same test every run, so runs compare fairly."""
    test = VC.corpus(2400, seed=424242)
    rng = np.random.default_rng(0)
    return [make_batch(test, rng) for _ in range(25)]


@torch.no_grad()
def test_loss(model, batches):
    model.eval()
    tot = 0.0
    for x, y in batches:
        tot += F.cross_entropy(model(x).view(-1, V), y.view(-1), ignore_index=-1).item()
    model.train()
    return tot / len(batches)


def main():
    run_no = int(os.environ.get("GITHUB_RUN_NUMBER", "0"))
    torch.manual_seed(SEED + run_no)
    rng = np.random.default_rng(SEED + run_no)
    os.makedirs(os.path.dirname(CKPT_OUT), exist_ok=True)
    # Each run makes fresh example lines, so continued training keeps seeing new ones.
    train = VC.corpus(40000, seed=SEED + run_no)
    evalb = make_eval_batches()
    model = TinyGPT()
    nparams = sum(p.numel() for p in model.parameters())
    print(f"vocab {V} words, {nparams} parameters")

    # Continue from the last kept model when there is one and it still fits (same words, same size).
    version, total_steps, old = 0, 0, None
    if os.path.exists(CKPT_IN) and os.environ.get("BLOOP_VOICE_FRESH") != "1":
        ck = torch.load(CKPT_IN, map_location="cpu")
        if ck.get("vocab") == VOCAB and ck.get("shape") == [D, H, L, T]:
            model.load_state_dict(ck["model"])
            version, total_steps = ck.get("version", 1), ck.get("total_steps", 0)
            old = test_loss(model, evalb)
            print(f"continuing from voice v{version} ({total_steps} steps so far), test loss {old:.4f}")
        else:
            print("the saved voice has different words or size - starting fresh")
    resumed = old is not None
    old_state = {k: v.clone() for k, v in model.state_dict().items()} if resumed else None

    # Fresh: warm up to a high learning rate. Continuing: gentler, so it refines instead of forgetting.
    lr = LR if not resumed else LR / 3
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=STEPS, pct_start=0.1 if not resumed else 0.05)
    t0 = time.time()
    losses = []
    write_status(state="training", step=0, steps=STEPS, loss=None, elapsed_s=0, params=nparams, vocab=V,
                 started=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), curve=[],
                 mode="continue" if resumed else "fresh", from_version=version if resumed else None,
                 old_test_loss=round(old, 4) if resumed else None,
                 run_url=os.environ.get("RUN_URL"), commit=os.environ.get("GITHUB_SHA", "")[:7] or None)
    last_write = 0.0
    for step in range(STEPS):
        x, y = make_batch(train, rng)
        loss = F.cross_entropy(model(x).view(-1, V), y.view(-1), ignore_index=-1)
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        sched.step()
        losses.append(loss.item())
        if step % 25 == 0 and time.time() - last_write > 5:
            last_write = time.time()
            write_status(step=step + 1, loss=round(float(np.mean(losses[-100:])), 4), elapsed_s=round(time.time() - t0),
                         curve=[round(float(np.mean(losses[max(0, i - 50):i + 1])), 4) for i in range(0, step + 1, 25)])
        if step % 250 == 0 or step == STEPS - 1:
            print(f"step {step:5d}  loss {np.mean(losses[-100:]):.3f}  ({time.time() - t0:.0f}s)")
    model.eval()
    final = float(np.mean(losses[-200:]))
    new = test_loss(model, evalb)
    # Keep the new model only if it does at least as well on the lines it never trained on.
    kept = "new" if not resumed or new <= old else "old"
    if kept == "old":
        model.load_state_dict(old_state)
        print(f"new test loss {new:.4f} is worse than {old:.4f} - keeping the old voice v{version}")
    else:
        version += 1
        total_steps += STEPS
        print(f"keeping the new voice v{version}: test loss {new:.4f}" + (f" (was {old:.4f})" if resumed else ""))
    model.eval()
    torch.save({"model": model.state_dict(), "vocab": VOCAB, "shape": [D, H, L, T],
                "version": version, "total_steps": total_steps}, CKPT_OUT)
    write_status(state="checking", step=STEPS, loss=round(final, 4), elapsed_s=round(time.time() - t0),
                 test_loss=round(new, 4), kept=kept, version=version, total_steps=total_steps,
                 curve=[round(float(np.mean(losses[max(0, i - 50):i + 1])), 4) for i in range(0, STEPS, 25)])

    P = export_params(model)

    # Check the 8-bit numpy model agrees with PyTorch on a few prompts.
    test_prompts = []
    prng = __import__("random").Random(99)
    for _ in range(32):
        f, flags = VC.sample_feelings(prng)
        ev = prng.choice(VC.EVENTS)
        topic = prng.choice(VC.PIP_TOPICS) if ev == "pip" else None
        test_prompts.append(encode(VC.prompt_tokens(f, flags, ev, topic)))
    agree = 0
    with torch.no_grad():
        for pr in test_prompts:
            ids = list(pr)
            while len(ids) < T:
                nxt = int(model(torch.tensor([ids]))[0, -1].argmax())
                if nxt == END:
                    break
                ids.append(nxt)
            agree += ids[len(pr):] == np_generate(P, pr, max_new=T)[:len(ids) - len(pr)]
    print(f"8-bit model matches full-precision greedy output on {agree}/{len(test_prompts)} prompts")

    # Reference outputs for the C test.
    with open(os.path.join(OUT, "voice_test.txt"), "w") as f:
        for pr in test_prompts:
            out = np_generate(P, pr, max_new=T)
            f.write(" ".join(map(str, pr)) + " | " + " ".join(map(str, out)) + "\n")

    samples = []
    for pr in test_prompts[:12]:
        words = [VOCAB[i] for i in pr if VOCAB[i] != "<say>"]
        samples.append({"feels": words, "says": detok(np_generate(P, pr, rng=rng, temp=0.8))})
    stats = {"steps": total_steps, "version": version, "test_loss": round(min(new, old) if kept == "old" else new, 4),
             "kept": kept, "loss": final, "params": nparams, "vocab": V, "agree": agree,
             "of": len(test_prompts), "seconds": round(time.time() - t0), "samples": samples,
             "curve": [round(float(np.mean(losses[max(0, i - 50):i + 1])), 4) for i in range(0, STEPS, 25)]}
    with open(os.path.join(OUT, "voice_samples.json"), "w") as f:
        json.dump(stats, f, indent=1)
    write_header(P, stats)
    write_status(state="trained", agree=agree, of=len(test_prompts), samples=samples)
    for s in samples:
        print(" ".join(s["feels"]), "->", s["says"])
    print(f"wrote {HEADER} ({os.path.getsize(HEADER) // 1024} KB)")


if __name__ == "__main__":
    main()
