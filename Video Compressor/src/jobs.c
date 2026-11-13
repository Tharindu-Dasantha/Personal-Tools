#include "app.h"

static void verify_done (GObject *source, GAsyncResult *res, gpointer user_data);
static void finish_job (Item *it, gboolean ok);
static gboolean spawn_step (Item *it);
static void sample_done (GObject *source, GAsyncResult *res, gpointer user_data);
static void encode_next (void);
static void learn_speed (Item *it);

/* ---- actions ---- */


static void
pause_job (Item *it)
{
    /* SIGSTOP freezes ffmpeg in place; SIGCONT (Start) resumes it. */
    g_subprocess_send_signal (it->proc, SIGSTOP);
    it->paused = TRUE;
    it->paused_at = g_get_monotonic_time ();
    render_row (it);
}

static void
resume_job (Item *it)
{
    g_subprocess_send_signal (it->proc, SIGCONT);
    it->paused = FALSE;
    it->paused_total += g_get_monotonic_time () - it->paused_at;
    render_row (it);
}

/* Every running job, paused (TRUE) or resumed. */
void
pause_all (gboolean pause)
{
    for (guint i = 0; app.n_jobs > 0 && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state == ST_ACTIVE && it->paused != pause)
            pause ? pause_job (it) : resume_job (it);
    }
    if (!pause && app.items != NULL)
        encode_next ();   /* slots that freed while paused */
}

static void
do_start (Item *one, gboolean sel)
{
    if (!is_busy ()) {   /* a new batch: the progress bar and summary count from here */
        for (guint i = 0; i < app.items->len; i++)
            ((Item *) app.items->pdata[i])->in_batch = FALSE;
        app.batch_t0 = g_get_monotonic_time ();
        app.batch_end = 0;
    }
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel))
            continue;
        if (one != NULL && it->state == ST_QUEUED) {
            it->prio = ++app.prio_seq;
            continue;
        }
        if (!can_start (it))
            continue;
        if (one == NULL && !sel && skip_in_bulk (it)) {
            set_status (it, "No saving · skipped",
                        "Likely no saving. Select it and press Start to compress it anyway.");
            continue;
        }
        if (it->state == ST_ACTIVE) {
            resume_job (it);
            continue;
        }
        g_autofree char *dir = g_path_get_dirname (it->path);
        g_autofree char *out_dir = g_build_filename (dir, vc_mode_out_dir (app.mode), NULL);
        if (g_mkdir_with_parents (out_dir, 0755) != 0) {
            alert ("Cannot create the output folder", g_strerror (errno), NULL);
            break;
        }
        it->state = ST_QUEUED;
        it->in_batch = TRUE;
        it->prio = one != NULL ? ++app.prio_seq : 0;
        set_status (it, NULL, NULL);
    }
    encode_next ();
}

static void
start_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) == 1)
        do_start (app.ask_one, app.ask_sel);
}

const char *
trash_checks (void)
{
    return app.mode == VC_MODE_IMAGES
               ? "\n\n✓ The same-type copy opens and has the expected size\n"
                 "✓ It is at least 5% smaller than the original\n"
                 "✓ Nothing is ever deleted permanently"
               : "\n\n✓ The compressed copy plays and has the same length\n"
                 "✓ It is at least 5% smaller than the original\n"
                 "✓ Nothing is ever deleted permanently";
}

/* The output that replaces the original: the compressed video, or an image's same-type copy. */
static char *
main_output (Item *it, const VcOptions *o)
{
    return app.mode == VC_MODE_IMAGES ? vc_image_outputs (it->path, it->probe, NULL)
                                      : vc_output_path (it->path, o->keep_subs, NULL);
}

void
act_start (Item *one, gboolean sel)
{
    guint n = 0;
    VcOptions o = current_options ();
    for (guint i = 0; trash_on () && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_start (it) || it->state == ST_ACTIVE
            || (one == NULL && !sel && skip_in_bulk (it)))
            continue;
        g_autofree char *out = main_output (it, &o);
        n += !g_file_test (out, G_FILE_TEST_EXISTS);
    }
    if (n == 0) {
        do_start (one, sel);
        return;
    }
    app.ask_one = one;
    app.ask_sel = sel;
    g_autofree char *message = g_strdup_printf ("Move up to %u original%s to Trash?", n,
                                                n == 1 ? "" : "s");
    g_autofree char *detail = g_strconcat ("Each original is moved only after its compressed copy is "
                                           "ready and checked. You can restore them from the Trash.",
                                           trash_checks (), NULL);
    confirm (message, detail, "Compress and Move to Trash", start_confirmed);
}

void
act_pause (Item *one, gboolean sel)
{
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_pause (it))
            continue;
        if (it->state == ST_ACTIVE) {
            pause_job (it);
        } else {
            it->state = ST_HELD;
            set_status (it, NULL, NULL);
        }
    }
    update_controls ();
}

void
act_stop (Item *one, gboolean sel)
{
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_stop (it))
            continue;
        if (it->state == ST_QUEUED || it->state == ST_HELD) {
            it->state = ST_NEW;
            it->in_batch = FALSE;
            set_status (it, NULL, NULL);
        } else {
            it->stop = TRUE;   /* checking: verify_done keeps the original */
            if (it->state == ST_ACTIVE) {
                /* SIGKILL also ends a paused (stopped) ffmpeg. finish_job removes the .part
                 * once ffmpeg is gone and has closed it, then starts the next queued item. */
                g_subprocess_force_exit (it->proc);
                render_row (it);
            }
        }
    }
    update_controls ();
}

/* ---- checking an output, then trashing its original ---- */

static void
keep_original (Item *it, const char *why)
{
    it->state = ST_DONE;
    it->extra = "Original kept";
    set_status (it, NULL, why);
    update_controls ();
}

static void
trash_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    Item *it = user_data;
    g_autoptr (GError) error = NULL;
    gboolean ok = g_file_trash_finish (G_FILE (source), res, &error);
    if (app.closing)
        return;
    it->stop = FALSE;   /* too late to stop: the trash call was already made */
    if (ok) {
        it->state = ST_TRASHED;
        it->extra = "Original in Trash";
        set_status (it, NULL, it->out);
        g_autofree char *name = basename_display (it->path);
        g_autofree char *detail = g_strdup_printf ("%s · restore it from Trash any time", name);
        show_toast ("user-trash-symbolic", "Original moved to Trash", detail);
    } else {
        it->state = ST_DONE;
        it->extra = "Original kept · trash failed";
        set_status (it, NULL, error->message);
    }
    update_controls ();
}

static void
verify_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    Item *it = user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    g_autoptr (GError) error = NULL;
    g_autoptr (VcProbe) probe = NULL;
    if (g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, &error)
        && g_subprocess_get_successful (proc))
        probe = vc_parse_probe (out, &error);

    if (app.closing)
        return;   /* never trash after the window is gone; the original stays */
    if (it->stop) {
        it->stop = FALSE;
        keep_original (it, "Stopped before the original was moved to Trash");
        return;
    }
    /* `out` only ever exists after ffmpeg exited 0 and the .part was renamed. */
    GStatBuf st;
    gboolean kept = g_stat (it->out, &st) == 0 && vc_output_is_worth_keeping (it->size, st.st_size);
    gboolean pass;
    if (app.mode == VC_MODE_IMAGES) {
        int w, h, tw, th, ow = probe != NULL ? probe->width : 0, oh = probe != NULL ? probe->height : 0;
        vc_image_expected_size (it->probe->width, it->probe->height, it->max_width, &w, &h);
        /* ffprobe gives a JPEG's stored size; with EXIF orientation 5-8 the upright copy is transposed */
        vc_image_expected_size (it->probe->height, it->probe->width, it->max_width, &tw, &th);
        pass = vc_image_should_trash (TRUE, TRUE, kept, w, h, ow, oh)
               || vc_image_should_trash (TRUE, TRUE, kept, tw, th, ow, oh);
    } else {
        pass = vc_should_trash_original (TRUE, TRUE, it->probe->duration,
                                         probe != NULL ? probe->duration : 0, kept);
    }
    if (!pass) {
        g_autofree char *why = g_strdup_printf (
            "Output check failed: %s", error != NULL ? error->message
                                       : !kept ? "output missing or not smaller"
                                       : app.mode == VC_MODE_IMAGES ? "size differs" : "duration differs");
        keep_original (it, why);
        return;
    }
    g_autoptr (GFile) file = g_file_new_for_path (it->path);
    g_file_trash_async (file, G_PRIORITY_DEFAULT, NULL, trash_done, it);
}

void
check_then_trash (Item *it)
{
    it->state = ST_CHECKING;
    it->stop = FALSE;
    set_status (it, NULL, NULL);
    g_auto (GStrv) argv = vc_build_ffprobe_argv (it->out);
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL)
        keep_original (it, error->message);
    else
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, verify_done, it);
}

void
trash_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) != 1)
        return;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (in_scope (it, NULL, TRUE) && can_trash (it))
            check_then_trash (it);
    }
    update_controls ();
}

/* ---- encoding ---- */

/* So galleries sort the copy by when the video was taken, not by today. Best effort: a wrong
 * date never loses data, and the output check that follows does not depend on it. */
static void
copy_mtime (const char *from, const char *to)
{
    g_autoptr (GFile) src = g_file_new_for_path (from);
    g_autoptr (GFile) dst = g_file_new_for_path (to);
    g_autoptr (GFileInfo) info = g_file_query_info (src, G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                                    G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
                                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info != NULL)
        g_file_set_attributes_from_info (dst, info, G_FILE_QUERY_INFO_NONE, NULL, NULL);
}

/* Total bytes of the files in a frames folder, and how many there are. */
static guint64
dir_usage (const char *path, guint *count)
{
    guint64 total = 0;
    *count = 0;
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    for (const char *name; dir != NULL && (name = g_dir_read_name (dir)) != NULL;) {
        g_autofree char *f = g_build_filename (path, name, NULL);
        GStatBuf st;
        if (g_stat (f, &st) == 0) {
            total += st.st_size;
            (*count)++;
        }
    }
    return total;
}

/* Images: the row's result lines (same type, then webp), from what is on disk now. `out` becomes the
 * same-type copy, else the webp, else NULL. */
static void
image_result (Item *it, GPtrArray *steps)
{
    g_autofree char *webp = NULL;
    g_autofree char *same = vc_image_outputs (it->path, it->probe, &webp);
    const char *outs[] = { same, webp };
    g_clear_pointer (&it->out, g_free);
    it->out_size = 0;
    it->n_res = 0;
    for (guint i = 0; i < G_N_ELEMENTS (outs) && outs[i] != NULL; i++) {
        gboolean planned = FALSE;   /* else it existed before this run */
        for (guint s = 0; s < steps->len; s++)
            planned |= g_strcmp0 (((VcStep *) steps->pdata[s])->output, outs[i]) == 0;
        GStatBuf st;
        gboolean exists = g_stat (outs[i], &st) == 0;
        g_strlcpy (it->res[i].ext, strrchr (outs[i], '.') + 1, sizeof it->res[i].ext);
        it->res[i].size = exists ? (guint64) st.st_size : 0;
        it->res[i].fresh = planned;
        it->n_res = i + 1;
        if (exists && it->out == NULL) {
            it->out = g_strdup (outs[i]);
            it->out_size = st.st_size;
        }
    }
    /* only the same-type copy may replace the original (SPEC-image-compress.md D6) */
    it->webp_only = it->out != NULL && strcmp (it->out, same) != 0;
    if (it->webp_only && trash_on ())
        it->extra = g_str_has_suffix (same, ".png") ? "Original kept · no png copy"
                                                    : "Original kept · no jpg copy";
}

/* Runs when the job's last step has ended (or one failed, or it was stopped): moves the outputs
 * into place, removes every other part, and sets the row. The caller starts the next item. */
static void
finish_job (Item *it, gboolean ok)
{
    if (ok && !it->stop && app.mode == VC_MODE_COMPRESS)
        learn_speed (it);
    g_autoptr (GPtrArray) steps = g_steal_pointer (&it->steps);
    g_autofree char *last_err = g_steal_pointer (&it->last_err);
    it->paused = FALSE;
    app.n_jobs--;
    app.batch_end = g_get_monotonic_time ();
    it->state = ST_NEW;

    g_autoptr (GError) error = NULL;
    g_autofree char *kept = NULL;
    guint64 kept_size = 0;
    for (guint i = 0; ok && !it->stop && error == NULL && i < steps->len; i++) {
        VcStep *s = steps->pdata[i];
        GStatBuf st;
        /* A compressed output (each image output on its own) that isn't at least 5% smaller is
         * dropped with the other parts below; web video keeps every output (its point is
         * compatibility, SPEC-web-video.md D2). */
        gboolean shrinks = app.mode == VC_MODE_COMPRESS || app.mode == VC_MODE_IMAGES;
        if (s->output == NULL || g_stat (s->part, &st) != 0
            || (shrinks && !vc_output_is_worth_keeping (it->size, st.st_size)))
            continue;
        g_autoptr (GFile) src = g_file_new_for_path (s->part);
        g_autoptr (GFile) dst = g_file_new_for_path (s->output);
        /* No OVERWRITE flag: an existing output is never replaced. */
        if (g_file_move (src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
            copy_mtime (it->path, s->output);
            g_free (kept);
            kept = g_strdup (s->output);
            kept_size = s->part_is_dir ? dir_usage (s->output, &it->n_frames) : (guint64) st.st_size;
        }
    }
    for (guint i = 0; i < steps->len; i++)   /* moved parts are already gone */
        vc_remove_part (((VcStep *) steps->pdata[i])->part);

    if (it->stop) {
        it->stop = FALSE;
        set_status (it, "Stopped", NULL);
    } else if (!ok || error != NULL) {
        set_status (it, "Failed", error != NULL ? error->message
                                  : last_err != NULL ? last_err : "ffmpeg failed");
    } else if (app.mode == VC_MODE_IMAGES) {
        it->state = ST_DONE;
        image_result (it, steps);
        if (it->out == NULL) {
            set_status (it, "Skipped – already optimized",
                        "No copy was at least 5% smaller than the original, so none was kept.");
        } else if (it->webp_only) {
            set_status (it, NULL, it->out);   /* image_result said why the original stays */
        } else {
            set_status (it, NULL, it->out);
            if (trash_on ())
                check_then_trash (it);
        }
    } else if (kept == NULL) {
        it->state = ST_DONE;   /* no output: nothing to trash, and no point encoding again */
        set_status (it, NULL, NULL);
    } else {
        it->state = ST_DONE;
        g_free (it->out);
        it->out = g_steal_pointer (&kept);
        it->out_size = kept_size;
        set_status (it, NULL, it->out);
        if (trash_on ())
            check_then_trash (it);   /* runs alongside the next encode */
    }
}


/* All three events of the current step are in: start the next step, or finish the job. */
static void
step_ended (Item *it)
{
    VcStep *s = it->steps->pdata[it->step];
    g_autoptr (GSubprocess) proc = g_steal_pointer (&it->proc);
    int code = g_subprocess_get_if_exited (proc) ? g_subprocess_get_exit_status (proc) : -1;
    /* Exit 0 must leave the part behind; a soft exit means "no output" and is fine. */
    gboolean ok = (code == 0 && g_file_test (s->part, G_FILE_TEST_EXISTS))
                  || (s->soft_exit != 0 && code == s->soft_exit);
    if (ok && !it->stop && it->step + 1 < it->steps->len) {
        it->step++;
        it->fraction = (double) it->step / it->steps->len;   /* steps without progress output */
        update_progress ();
        if (spawn_step (it))
            return;
        ok = FALSE;
    }
    finish_job (it, ok);
    encode_next ();
}

static void
job_step_done (Item *it)
{
    if (--it->pending == 0 && !app.closing)   /* on_close already removed the parts */
        step_ended (it);
}

static void
on_progress_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDataInputStream *ds = G_DATA_INPUT_STREAM (source);
    g_autofree char *line = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);
    if (line == NULL) {
        g_object_unref (ds);
        job_step_done (user_data);
        return;
    }
    if (!app.closing) {   /* after close, keep draining to EOF but leave the widgets alone */
        Item *it = user_data;
        VcStep *s = it->steps->pdata[it->step];
        double fraction, dur = s->duration > 0 ? s->duration : it->probe->duration;
        if (s->progress && vc_parse_progress_line (line, dur, &fraction)) {
            it->fraction = (it->step + fraction) / it->steps->len;
            render_row (it);   /* bar, percent and the time left under it */
            update_progress ();
        } else if (s->progress && dur <= 0 && g_str_has_prefix (line, "progress=")) {
            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (it->bar));
        }
    }
    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_progress_line,
                                         user_data);
}

/* Keeps the last stderr line for the Failed tooltip; draining also stops ffmpeg blocking. */
static void
on_stderr_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDataInputStream *ds = G_DATA_INPUT_STREAM (source);
    g_autofree char *line = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);
    Item *it = user_data;
    if (line == NULL) {
        g_object_unref (ds);
        job_step_done (it);
        return;
    }
    if (*g_strstrip (line) != '\0') {
        g_free (it->last_err);
        it->last_err = g_utf8_make_valid (line, -1);
    }
    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_stderr_line,
                                         user_data);
}

static void
on_ffmpeg_exit (GObject *source, GAsyncResult *res, gpointer user_data)
{
    g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
    job_step_done (user_data);
}

/* Pulses the running steps that have no progress output; ends when there are none. */
gboolean
pulse_bar (gpointer data)
{
    (void) data;
    gboolean any = FALSE;
    for (guint i = 0; !app.closing && app.n_jobs > 0 && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state != ST_ACTIVE || ((VcStep *) it->steps->pdata[it->step])->progress)
            continue;
        any = TRUE;
        if (!it->paused)
            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (it->bar));
    }
    if (!any)
        app.pulse_id = 0;
    return any ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* Runs in the child before exec: low CPU and idle disk priority, so the desktop wins while you
 * use the laptop and the encode runs at full speed when you don't. The PID stays ffmpeg's. */
static void
lower_priority (gpointer data)
{
    (void) data;
    setpriority (PRIO_PROCESS, 0, 10);
    syscall (SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
}

/* Starts the job's current step. FALSE (with it->last_err set) when it can't be started. */
static gboolean
spawn_step (Item *it)
{
    VcStep *s = it->steps->pdata[it->step];
    g_autoptr (GError) error = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup (launcher, lower_priority, NULL, NULL);
    g_clear_pointer (&it->last_err, g_free);
    /* A killed run may have left the part (pngquant won't overwrite one). ffmpeg writes frames
     * into the folder but doesn't create it. */
    if (!vc_remove_part (s->part) || (s->part_is_dir && g_mkdir (s->part, 0755) != 0)) {
        g_autofree char *display = g_filename_display_name (s->part);
        it->last_err = g_strdup_printf ("Cannot create %s", display);
        return FALSE;
    }
    it->proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) s->argv, &error);
    if (it->proc == NULL) {
        it->last_err = g_strdup (error->message);
        return FALSE;
    }
    if (it->paused)   /* paused just as the previous step ended */
        g_subprocess_send_signal (it->proc, SIGSTOP);
    it->pending = 3;
    if (!s->progress && app.pulse_id == 0)
        app.pulse_id = g_timeout_add (100, pulse_bar, NULL);

    GDataInputStream *so = g_data_input_stream_new (g_subprocess_get_stdout_pipe (it->proc));
    GDataInputStream *se = g_data_input_stream_new (g_subprocess_get_stderr_pipe (it->proc));
    g_data_input_stream_read_line_async (so, G_PRIORITY_DEFAULT, NULL, on_progress_line, it);
    g_data_input_stream_read_line_async (se, G_PRIORITY_DEFAULT, NULL, on_stderr_line, it);
    g_subprocess_wait_async (it->proc, NULL, on_ffmpeg_exit, it);
    return TRUE;
}

/* Takes `steps`. The caller starts the next item if this one can't start. */
static void
start_job (Item *it, GPtrArray *steps)
{
    app.n_jobs++;
    it->t0 = g_get_monotonic_time ();
    it->paused = FALSE;
    it->paused_total = 0;
    it->steps = steps;
    it->step = 0;
    it->state = ST_ACTIVE;
    it->stop = FALSE;
    it->fraction = 0;
    it->n_res = 0;
    it->webp_only = FALSE;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), 0);
    set_status (it, NULL, NULL);
    if (!spawn_step (it))
        finish_job (it, FALSE);
}

/* ---- GPU size samples ---- */

/* Limits, so a big folder never costs much: one sample at a time at low priority, only while
 * nothing is queued, encoding or being checked, biggest videos first, at most SAMPLE_MAX per
 * folder and settings (the rest keep the formula), each killed after SAMPLE_TIMEOUT, and none
 * after 3 failures in a row. Changing folder or closing the window cancels the one in flight. */
#define SAMPLE_MAX 24
#define SAMPLE_TIMEOUT 15

void
sample_stop (void)
{
    g_clear_handle_id (&app.sample_timer, g_source_remove);
    g_cancellable_cancel (app.sample_cancel);   /* sample_done then leaves the item alone */
    g_clear_object (&app.sample_cancel);
    if (app.sample_proc != NULL) {
        g_subprocess_force_exit (app.sample_proc);
        g_subprocess_wait (app.sample_proc, NULL, NULL);
        g_clear_object (&app.sample_proc);
    }
    app.sample_item = NULL;
}

static void
sample_timed_out (gpointer data)
{
    (void) data;
    app.sample_timer = 0;
    if (app.sample_proc != NULL)
        g_subprocess_force_exit (app.sample_proc);   /* sample_done counts it as a failure */
}

static guint64 sample_bytes;   /* the sample's output so far: only its size matters */

static void
sample_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GError) error = NULL;
    gboolean ok = g_subprocess_wait_finish (G_SUBPROCESS (source), res, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;   /* sample_stop ran: the folder changed or the window closed */
    g_clear_handle_id (&app.sample_timer, g_source_remove);
    Item *it = g_steal_pointer (&app.sample_item);
    guint64 n = sample_bytes;
    ok = ok && g_subprocess_get_successful (G_SUBPROCESS (source)) && n > 0;
    g_clear_object (&app.sample_proc);

    it->sampled = TRUE;
    it->sample_side = app.sample_side;
    it->sample_fps = app.sample_fps;
    it->sample_kbps = ok ? n * 8.0 / 1000.0 / app.sample_secs : 0;
    app.sample_fails = ok ? 0 : app.sample_fails + 1;
    render_row (it);
    update_speed_guide ();
    update_controls ();   /* which starts the next one */
}

/* Reads and discards the sample's stdout in 1 MiB chunks, counting bytes; at the end, waits for
 * the exit status. */
static void
sample_read (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GError) error = NULL;
    gssize n = g_input_stream_skip_finish (G_INPUT_STREAM (source), res, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (n > 0) {
        sample_bytes += n;
        g_input_stream_skip_async (G_INPUT_STREAM (source), 1 << 20, G_PRIORITY_LOW, app.sample_cancel,
                                   sample_read, NULL);
    } else {
        g_subprocess_wait_async (app.sample_proc, app.sample_cancel, sample_done, NULL);
    }
}

void
sample_next (void)
{
    if (!app.gpu_ok || app.closing || app.mode != VC_MODE_COMPRESS || app.items == NULL
        || app.sample_proc != NULL
        || app.probes_done < app.items->len || is_busy ())
        return;
    VcOptions o = current_options ();
    if (o.max_short_side != app.sample_side || o.max_fps != app.sample_fps) {
        app.sample_side = o.max_short_side;   /* new settings: a fresh allowance */
        app.sample_fps = o.max_fps;
        app.samples_taken = app.sample_fails = 0;
    }
    if (app.samples_taken >= SAMPLE_MAX || app.sample_fails >= 3)
        return;
    Item *best = NULL;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state == ST_NEW && it->probe != NULL && it->probe->duration > 0
            && !sampled_at (it, &o) && (best == NULL || it->size > best->size))
            best = it;
    }
    if (best == NULL)
        return;

    g_auto (GStrv) argv = vc_build_sample_argv (best->path, best->probe, &o, &app.sample_secs);
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_child_setup (launcher, lower_priority, NULL, NULL);
    app.samples_taken++;
    app.sample_proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) argv, NULL);
    if (app.sample_proc == NULL) {
        best->sampled = TRUE;   /* kbps 0: it keeps the formula */
        best->sample_side = o.max_short_side;
        best->sample_fps = o.max_fps;
        app.sample_fails++;
        sample_next ();   /* ends after 3 failures */
        return;
    }
    app.sample_item = best;
    if (app.sample_cancel == NULL)
        app.sample_cancel = g_cancellable_new ();
    app.sample_timer = g_timeout_add_seconds_once (SAMPLE_TIMEOUT, sample_timed_out, NULL);
    sample_bytes = 0;
    g_input_stream_skip_async (g_subprocess_get_stdout_pipe (app.sample_proc), 1 << 20, G_PRIORITY_LOW,
                               app.sample_cancel, sample_read, NULL);
}

/* Starts queued items while fewer than max_jobs() run and none is paused (the rest of the queue
 * waits for a paused one): the one with the highest "Compress next" priority, otherwise the first
 * in list order. */
static void
encode_next (void)
{
    VcOptions o = current_options ();
    while (app.n_jobs < max_jobs () && paused_jobs (NULL) == 0) {
        Item *it = NULL;
        for (guint i = 0; i < app.items->len; i++) {
            Item *c = app.items->pdata[i];
            if (c->state == ST_QUEUED && (it == NULL || c->prio > it->prio))
                it = c;
        }
        if (it == NULL)
            break;
        it->prio = 0;
        it->max_width = o.max_width;
        g_autofree char *reason = NULL;
        GPtrArray *steps = vc_mode_steps (app.mode, it->path, it->probe, &o, &reason);
        if (steps == NULL && app.mode == VC_MODE_FRAMES) {
            it->state = ST_DONE;
            g_clear_pointer (&it->out, g_free);
            if (g_strcmp0 (reason, "starts after the end") == 0) {
                set_status (it, "Starts after the end", "Start is past this video's end, so it was skipped.");
                continue;
            }
            it->out = vc_frames_output (it->path, NULL);
            it->out_size = dir_usage (it->out, &it->n_frames);
            it->extra = "Already extracted";
            set_status (it, NULL, reason);
            continue;
        }
        if (steps == NULL) {
            it->state = ST_DONE;
            g_free (it->out);
            /* an earlier finished output, so its original can be trashed */
            it->out = app.mode == VC_MODE_WEB_VIDEO ? vc_web_video_output (it->path, NULL)
                                                    : main_output (it, &o);
            GStatBuf st;
            it->out_size = g_stat (it->out, &st) == 0 ? st.st_size : 0;
            it->extra = app.mode == VC_MODE_WEB_VIDEO ? "Already converted" : "Already compressed";
            set_status (it, NULL, reason);
            continue;
        }
        start_job (it, steps);   /* takes steps */
    }
    update_controls ();
}

/* Encode speed, in output pixel-frames per second, learned from finished encodes on this machine
 * and kept in ~/.config/media-tools/speed.ini. Seeded from this laptop's benchmark (i5-1240P,
 * research/handbrake/adopt/16-encode-speed.md), so a new machine's first estimate is rough until
 * one video has finished. */
#define SEED_MEDIUM_PPS 2.8e7

static char *
speed_file (void)
{
    return g_build_filename (g_get_user_config_dir (), "media-tools", "speed.ini", NULL);
}

void
load_speeds (void)
{
    app.medium_pps = SEED_MEDIUM_PPS;
    app.gpu_pps = 0;   /* vc_estimate_seconds then uses the GPU's time factor */
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_autofree char *f = speed_file ();
    if (!g_key_file_load_from_file (k, f, G_KEY_FILE_NONE, NULL))
        return;
    double x = g_key_file_get_double (k, "speed", "x265-medium", NULL);
    double g = g_key_file_get_double (k, "speed", "gpu", NULL);
    if (x > 0 && isfinite (x))
        app.medium_pps = x;
    if (g > 0 && isfinite (g))
        app.gpu_pps = g;
}

/* After a compress encode ended well: how fast it really went, averaged into what is stored. */
static void
learn_speed (Item *it)
{
    VcOptions o = current_options ();   /* settings are locked while busy: these encoded it */
    double secs = (g_get_monotonic_time () - it->t0 - it->paused_total) / 1e6;
    double pf = vc_output_pixel_frames (it->probe, &o);
    if (secs < 3 || pf <= 0)
        return;   /* too short to time well */
    double t, sz;
    vc_preset_factors (o.preset, &t, &sz);
    gboolean gpu = strcmp (o.preset, VC_PRESET_GPU) == 0;
    double measured = gpu ? pf / secs : pf / secs * t;   /* x265 as its medium-speed equivalent */
    const char *key = gpu ? "gpu" : "x265-medium";
    double *slot = gpu ? &app.gpu_pps : &app.medium_pps;
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_autofree char *f = speed_file ();
    g_key_file_load_from_file (k, f, G_KEY_FILE_KEEP_COMMENTS, NULL);
    /* the first real encode replaces the seed; later ones average in */
    *slot = g_key_file_has_key (k, "speed", key, NULL) ? (*slot + measured) / 2 : measured;
    g_key_file_set_double (k, "speed", key, *slot);
    g_autofree char *dir = g_path_get_dirname (f);
    if (g_mkdir_with_parents (dir, 0755) == 0)
        g_key_file_save_to_file (k, f, NULL);   /* best effort: only estimates depend on it */
}
