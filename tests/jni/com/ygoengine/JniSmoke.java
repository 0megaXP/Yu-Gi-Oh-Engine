package com.ygoengine;

import java.nio.file.Files;
import java.nio.file.Path;

/** Exercises the JNI bridge: load, analyze with progress polling, cancel, profile, side. */
public final class JniSmoke {
    public static void main(String[] args) throws Exception {
        String pack = args[0], examples = args[1];
        long h = YgoNative.create(pack, pack);
        String err = YgoNative.error(h);
        if (!err.isEmpty()) throw new IllegalStateException(err);

        String req = Files.readString(Path.of(examples, "request_analyze.json"));
        final String[] result = new String[1];
        Thread t = new Thread(() -> result[0] = YgoNative.analyze(h, req));
        t.start();
        while (t.isAlive()) {
            System.out.println("progress: " + YgoNative.progress(h).replace('\t', ' '));
            t.join(1000);
        }
        check(result[0].contains("\"lines\":[{"), "analyze returned lines");
        check(result[0].contains("\"cancelled\":false"), "not cancelled");

        // Cancel shortly after starting a long search: must return quickly with partial results.
        String longReq = req.replace("\"iterations\": 800", "\"iterations\": 200000");
        Thread t2 = new Thread(() -> result[0] = YgoNative.analyze(h, longReq));
        long start = System.nanoTime();
        t2.start();
        Thread.sleep(1500);
        YgoNative.cancel(h);
        t2.join();
        double secs = (System.nanoTime() - start) / 1e9;
        check(result[0].contains("\"cancelled\":true"), "cancel flag set");
        check(secs < 6, "cancel returned in " + String.format("%.1f", secs) + "s");

        String prof = YgoNative.profile(h, Files.readString(Path.of(examples, "request_profile.json")));
        check(prof.contains("\"key_cards\""), "profile");
        String side = YgoNative.side(h, Files.readString(Path.of(examples, "request_side.json")));
        check(side.contains("\"going_second\""), "side");
        String ref = YgoNative.reference(h, Files.readString(Path.of(examples, "request_reference.json"))
            .replace("\"hands\": 6", "\"hands\": 2").replace("\"iterations\": 800", "\"iterations\": 200"));
        check(ref.contains("\"samples\":[{") && ref.contains("\"common_cards\""), "reference boards");
        check(YgoNative.analyze(h, "{not json").contains("\"error\""), "bad request gives error");

        // Live duel: bots play a few turns; a "person" answers one decision by index.
        String duelReq = Files.readString(Path.of(examples, "request_duel.json"));
        long d = YgoNative.liveCreate(h, duelReq);
        check(d != 0, "live duel created " + YgoNative.lastError());
        String st = YgoNative.liveState(d);
        check(st.contains("\"decision\":{") && st.contains("\"monsters\""), "live state");
        check(YgoNative.liveRespond(d, 0), "respond to first decision");
        check(!YgoNative.liveRespond(d, 999), "illegal answer rejected");
        int moves = 0;
        while (moves < 400 && !YgoNative.liveState(d).contains("\"turn\":4,")) {
            if (YgoNative.liveBot(d, moves % 2 == 0 ? 0 : 1).isEmpty()) break;
            moves++;
        }
        check(moves > 10, "bots made " + moves + " moves");
        YgoNative.liveDestroy(d);
        check(YgoNative.liveCreate(h, "{\"decks\":[]}") == 0 && !YgoNative.lastError().isEmpty(), "bad duel request rejected");
        YgoNative.destroy(h);
        System.out.println("JNI smoke test passed");
    }

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) System.exit(1);
    }
}
