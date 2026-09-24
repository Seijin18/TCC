# pairing-handshake-hardening

Makes the mestre/escravo pairing handshake robust: known-good radio state on every entry into pairing, session nonce + idempotent re-ack (no more half-open states), rendezvous timing that guarantees overlap, and a pairing-candidate blacklist that only blames channels on real evidence
