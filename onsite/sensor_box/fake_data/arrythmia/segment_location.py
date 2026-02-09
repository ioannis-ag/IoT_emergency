import wfdb

record_name = "418"  # VFDB record IDs include: 418, 419, ... 615
rec = wfdb.rdrecord(record_name, pn_dir="vfdb")

print("Record:", rec.record_name)
print("Fs (Hz):", rec.fs)
print("Signals shape:", rec.p_signal.shape)  # samples x channels
print("Channel names:", rec.sig_name)
