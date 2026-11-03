from pathlib import Path

root = Path(__file__).resolve().parents[1]
regvm = (root / "src/interpreter/regvm.c").read_text()
slow = (root / "src/interpreter/regvm_slow.c").read_text()

# The hot loop must not eagerly publish source IP on every dispatch anymore.
assert "        frame->ip = source_ip[here];\n\n        switch(op)" not in regvm
assert "#define REG_SYNC_IP()" in regvm
assert "RegVM__slow_error" in regvm
assert "PK_REGVM_HOT PK_REGVM_ALIGN64 FrameResult VM__run_top_frame_reg" in regvm
assert "const int32_t code_length = rc->code.length;" in regvm
assert "RegRuntimeSite* runtime_sites = (RegRuntimeSite*)rc->runtime_sites.data;" in regvm
assert "PK_REGVM_ASSUME((unsigned)op < RBC__COUNT);" in regvm
assert "const bool profile_enabled = self->regjit_hooks != NULL;" in regvm

# Slow semantic helpers live in a separate translation unit so the main dispatch
# body can stay smaller under LTO/PGO and cold/noinline attributes.
for name in (
    "RegVM__slow_binary",
    "RegVM__slow_contains",
    "RegVM__slow_build_imag",
    "RegVM__slow_build_string",
    "RegVM__slow_unpack_sequence",
    "RegVM__slow_unpack_ex",
    "RegVM__slow_error",
):
    assert name in slow
assert "PK_CEVAL_COLD" in slow

print("hotloop layout smoke: ok")
