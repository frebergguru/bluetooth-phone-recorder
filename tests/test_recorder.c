/*
 * Recorder: files open back with libsndfile, tracks are aligned and have the
 * right length, markers are written. Runs in the build folder.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <dirent.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rec/recorder.h"
#include "testutil.h"

static void check_session(const char *dir, const char *ext, int frames_expected)
{
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    if (!d) return;
    int files = 0;
    bool labels = false;
    struct dirent *de;
    while ((de = readdir(d))) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (strcmp(de->d_name, "markers-audacity.txt") == 0) {
            FILE *f = fopen(path, "r");
            char line[256] = "";
            if (f) {
                if (!fgets(line, sizeof line, f)) line[0] = 0;
                fclose(f);
            }
            labels = strstr(line, "Intro") != NULL;
            continue;
        }
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot + 1, ext) != 0) continue;
        SF_INFO info = {0};
        SNDFILE *sf = sf_open(path, SFM_READ, &info);
        CHECK(sf != NULL);
        if (!sf) continue;
        files++;
        CHECK(info.samplerate == 48000);
        CHECK(info.frames == frames_expected);
        float buf[4];
        sf_seek(sf, 4800, SEEK_SET);
        sf_readf_float(sf, buf, 1);
        CHECK(fabsf(buf[0]) > 0.0f || strstr(de->d_name, "silent"));
        sf_close(sf);
    }
    closedir(d);
    CHECK(files == 3);
    CHECK(labels);
}

static int record(rec_format_t fmt, char *dir_out, size_t len)
{
    recorder_t *r = recorder_new();
    rec_track_spec_t tracks[3] = {{"full-mix", 2}, {"caller", 1}, {"Mic 1", 1}};
    char err[256];
    mkdir("test-recordings", 0755);
    if (recorder_start(r, "test-recordings", fmt, 48000, tracks, 3, err, sizeof err) != 0) {
        fprintf(stderr, "start: %s\n", err);
        return -1;
    }
    snprintf(dir_out, len, "%s", recorder_session_dir(r));
    float stereo[480], mono[240];
    for (int b = 0; b < 400; b++) { /* 2 s */
        for (int i = 0; i < 240; i++) {
            float v = 0.25f * sinf((float)(b * 240 + i) * 0.05f);
            stereo[2 * i] = stereo[2 * i + 1] = v;
            mono[i] = v;
        }
        recorder_push(r, 0, stereo, 240);
        recorder_push(r, 1, mono, 240);
        recorder_push(r, 2, mono, 240);
        recorder_block_done(r, 240);
        if (b == 100) recorder_add_marker(r, "Intro");
    }
    recorder_stop(r);
    CHECK(recorder_overruns(r) == 0);
    recorder_free(r);
    return 0;
}

int main(void)
{
    char dir[1024];
    CHECK(record(REC_FMT_WAV_FLOAT, dir, sizeof dir) == 0);
    check_session(dir, "wav", 96000);
    sleep(1); /* session folders are named by the second */
    CHECK(record(REC_FMT_FLAC_24, dir, sizeof dir) == 0);
    check_session(dir, "flac", 96000);
    TEST_DONE();
}
