/* Copyright (c) 2006-2014 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/argv.h"
#include "tig/repo.h"
#include "tig/watch.h"
#include "tig/options.h"
#include "tig/view.h"
#include "tig/draw.h"
#include "tig/display.h"

/*
 * Navigation
 */

bool
goto_view_line(struct view *view, unsigned long offset, unsigned long lineno)
{
	if (lineno >= view->lines)
		lineno = view->lines > 0 ? view->lines - 1 : 0;

	if (offset > lineno || offset + view->height <= lineno) {
		unsigned long half = view->height / 2;

		if (lineno > half)
			offset = lineno - half;
		else
			offset = 0;
	}

	if (offset != view->pos.offset || lineno != view->pos.lineno) {
		view->pos.offset = offset;
		view->pos.lineno = lineno;
		return TRUE;
	}

	return FALSE;
}

/* Scrolling backend */
void
do_scroll_view(struct view *view, int lines)
{
	bool redraw_current_line = FALSE;

	/* The rendering expects the new offset. */
	view->pos.offset += lines;

	assert(0 <= view->pos.offset && view->pos.offset < view->lines);
	assert(lines);

	/* Move current line into the view. */
	if (view->pos.lineno < view->pos.offset) {
		view->pos.lineno = view->pos.offset;
		redraw_current_line = TRUE;
	} else if (view->pos.lineno >= view->pos.offset + view->height) {
		view->pos.lineno = view->pos.offset + view->height - 1;
		redraw_current_line = TRUE;
	}

	assert(view->pos.offset <= view->pos.lineno && view->pos.lineno < view->lines);

	/* Redraw the whole screen if scrolling is pointless. */
	if (view->height < ABS(lines)) {
		redraw_view(view);

	} else {
		int line = lines > 0 ? view->height - lines : 0;
		int end = line + ABS(lines);

		scrollok(view->win, TRUE);
		wscrl(view->win, lines);
		scrollok(view->win, FALSE);

		while (line < end && draw_view_line(view, line))
			line++;

		if (redraw_current_line)
			draw_view_line(view, view->pos.lineno - view->pos.offset);
		wnoutrefresh(view->win);
	}

	view->has_scrolled = TRUE;
	report_clear();
}

/* Scroll frontend */
void
scroll_view(struct view *view, enum request request)
{
	int lines = 1;

	assert(view_is_displayed(view));

	if (request == REQ_SCROLL_WHEEL_DOWN || request == REQ_SCROLL_WHEEL_UP)
		lines = opt_mouse_scroll;

	switch (request) {
	case REQ_SCROLL_FIRST_COL:
		view->pos.col = 0;
		redraw_view_from(view, 0);
		report_clear();
		return;
	case REQ_SCROLL_LEFT:
		if (view->pos.col == 0) {
			report("Cannot scroll beyond the first column");
			return;
		}
		if (view->pos.col <= apply_step(opt_horizontal_scroll, view->width))
			view->pos.col = 0;
		else
			view->pos.col -= apply_step(opt_horizontal_scroll, view->width);
		redraw_view_from(view, 0);
		report_clear();
		return;
	case REQ_SCROLL_RIGHT:
		view->pos.col += apply_step(opt_horizontal_scroll, view->width);
		redraw_view(view);
		report_clear();
		return;
	case REQ_SCROLL_PAGE_DOWN:
		lines = view->height;
	case REQ_SCROLL_WHEEL_DOWN:
	case REQ_SCROLL_LINE_DOWN:
		if (view->pos.offset + lines > view->lines)
			lines = view->lines - view->pos.offset;

		if (lines == 0 || view->pos.offset + view->height >= view->lines) {
			report("Cannot scroll beyond the last line");
			return;
		}
		break;

	case REQ_SCROLL_PAGE_UP:
		lines = view->height;
	case REQ_SCROLL_LINE_UP:
	case REQ_SCROLL_WHEEL_UP:
		if (lines > view->pos.offset)
			lines = view->pos.offset;

		if (lines == 0) {
			report("Cannot scroll beyond the first line");
			return;
		}

		lines = -lines;
		break;

	default:
		die("request %d not handled in switch", request);
	}

	do_scroll_view(view, lines);
}

/* Cursor moving */
void
move_view(struct view *view, enum request request)
{
	int scroll_steps = 0;
	int steps;

	switch (request) {
	case REQ_MOVE_FIRST_LINE:
		steps = -view->pos.lineno;
		break;

	case REQ_MOVE_LAST_LINE:
		steps = view->lines - view->pos.lineno - 1;
		break;

	case REQ_MOVE_PAGE_UP:
		steps = view->height > view->pos.lineno
		      ? -view->pos.lineno : -view->height;
		break;

	case REQ_MOVE_PAGE_DOWN:
		steps = view->pos.lineno + view->height >= view->lines
		      ? view->lines - view->pos.lineno - 1 : view->height;
		break;

	case REQ_MOVE_HALF_PAGE_UP:
		steps = view->height / 2 > view->pos.lineno
		      ? -view->pos.lineno : -(view->height / 2);
		break;

	case REQ_MOVE_HALF_PAGE_DOWN:
		steps = view->pos.lineno + view->height / 2 >= view->lines
		      ? view->lines - view->pos.lineno - 1 : view->height / 2;
		break;

	case REQ_MOVE_UP:
	case REQ_PREVIOUS:
		steps = -1;
		break;

	case REQ_MOVE_DOWN:
	case REQ_NEXT:
		steps = 1;
		break;

	default:
		die("request %d not handled in switch", request);
	}

	if (steps <= 0 && view->pos.lineno == 0) {
		report("Cannot move beyond the first line");
		return;

	} else if (steps >= 0 && view->pos.lineno + 1 >= view->lines) {
		report("Cannot move beyond the last line");
		return;
	}

	/* Move the current line */
	view->pos.lineno += steps;
	assert(0 <= view->pos.lineno && view->pos.lineno < view->lines);

	/* Check whether the view needs to be scrolled */
	if (view->pos.lineno < view->pos.offset ||
	    view->pos.lineno >= view->pos.offset + view->height) {
		scroll_steps = steps;
		if (steps < 0 && -steps > view->pos.offset) {
			scroll_steps = -view->pos.offset;

		} else if (steps > 0) {
			if (view->pos.lineno == view->lines - 1 &&
			    view->lines > view->height) {
				scroll_steps = view->lines - view->pos.offset - 1;
				if (scroll_steps >= view->height)
					scroll_steps -= view->height - 1;
			}
		}
	}

	if (!view_is_displayed(view)) {
		view->pos.offset += scroll_steps;
		assert(0 <= view->pos.offset && view->pos.offset < view->lines);
		view->ops->select(view, &view->line[view->pos.lineno]);
		return;
	}

	/* Repaint the old "current" line if we be scrolling */
	if (ABS(steps) < view->height)
		draw_view_line(view, view->pos.lineno - steps - view->pos.offset);

	if (scroll_steps) {
		do_scroll_view(view, scroll_steps);
		return;
	}

	/* Draw the current line */
	draw_view_line(view, view->pos.lineno - view->pos.offset);

	wnoutrefresh(view->win);
	report_clear();
}

/*
 * Searching
 */

DEFINE_ALLOCATOR(realloc_unsigned_ints, unsigned int, 32)

bool
grep_text(struct view *view, const char *text[])
{
	regmatch_t pmatch;
	size_t i;

	for (i = 0; text[i]; i++)
		if (*text[i] && !regexec(view->regex, text[i], 1, &pmatch, 0))
			return TRUE;
	return FALSE;
}

void
select_view_line(struct view *view, unsigned long lineno)
{
	struct position old = view->pos;

	if (goto_view_line(view, view->pos.offset, lineno)) {
		if (view_is_displayed(view)) {
			if (old.offset != view->pos.offset) {
				redraw_view(view);
			} else {
				draw_view_line(view, old.lineno - view->pos.offset);
				draw_view_line(view, view->pos.lineno - view->pos.offset);
				wnoutrefresh(view->win);
			}
		} else {
			view->ops->select(view, &view->line[view->pos.lineno]);
		}
	}
}

static bool
find_matches(struct view *view)
{
	size_t lineno;

	/* Note, lineno is unsigned long so will wrap around in which case it
	 * will become bigger than view->lines. */
	for (lineno = 0; lineno < view->lines; lineno++) {
		if (!view->ops->grep(view, &view->line[lineno]))
			continue;

		if (!realloc_unsigned_ints(&view->matched_line, view->matched_lines, 1))
			return FALSE;

		view->matched_line[view->matched_lines++] = lineno;
	}

	return TRUE;
}

void
find_next(struct view *view, enum request request)
{
	int direction;
	size_t i;

	if (!*view->grep) {
		if (!*view->env->search)
			report("No previous search");
		else
			search_view(view, request);
		return;
	}

	switch (request) {
	case REQ_SEARCH:
	case REQ_FIND_NEXT:
		direction = 1;
		break;

	case REQ_SEARCH_BACK:
	case REQ_FIND_PREV:
		direction = -1;
		break;

	default:
		return;
	}

	if (!view->matched_lines && !find_matches(view)) {
		report("Allocation failure");
		return;
	}

	/* Note, `i` is unsigned and will wrap around in which case it
	 * will become bigger than view->matched_lines. */
	i = direction > 0 ? 0 : view->matched_lines - 1;
	for (; i < view->matched_lines; i += direction) {
		size_t lineno = view->matched_line[i];

		if (direction > 0 && lineno <= view->pos.lineno)
			continue;

		if (direction < 0 && lineno >= view->pos.lineno)
			continue;

		select_view_line(view, lineno);
		report("Line %zu matches '%s' (%zu of %zu)", lineno + 1, view->grep, i + 1, view->matched_lines);
		return;
	}

	report("No match found for '%s'", view->grep);
}

static void
reset_matches(struct view *view)
{
	free(view->matched_line);
	view->matched_line = NULL;
	view->matched_lines = 0;
}

void
search_view(struct view *view, enum request request)
{
	int regex_err;
	int regex_flags = opt_ignore_case ? REG_ICASE : 0;

	if (view->regex) {
		regfree(view->regex);
		*view->grep = 0;
	} else {
		view->regex = calloc(1, sizeof(*view->regex));
		if (!view->regex)
			return;
	}

	regex_err = regcomp(view->regex, view->env->search, REG_EXTENDED | regex_flags);
	if (regex_err != 0) {
		char buf[SIZEOF_STR] = "unknown error";

		regerror(regex_err, view->regex, buf, sizeof(buf));
		report("Search failed: %s", buf);
		return;
	}

	string_copy(view->grep, view->env->search);

	reset_matches(view);

	find_next(view, request);
}

/*
 * View history
 */

static bool
view_history_is_empty(struct view_history *history)
{
	return !history->stack;
}

struct view_state *
push_view_history_state(struct view_history *history, struct position *position, void *data)
{
	struct view_state *state = history->stack;

	if (state && data && history->state_alloc &&
	    !memcmp(state->data, data, history->state_alloc))
		return NULL;

	state = calloc(1, sizeof(*state) + history->state_alloc);
	if (!state)
		return NULL;

	state->prev = history->stack;
	history->stack = state;
	clear_position(&history->position);
	state->position = *position;
	state->data = &state[1];
	if (data && history->state_alloc)
		memcpy(state->data, data, history->state_alloc);
	return state;
}

bool
pop_view_history_state(struct view_history *history, struct position *position, void *data)
{
	struct view_state *state = history->stack;

	if (view_history_is_empty(history))
		return FALSE;

	history->position = state->position;
	history->stack = state->prev;

	if (data && history->state_alloc)
		memcpy(data, state->data, history->state_alloc);
	if (position)
		*position = state->position;

	free(state);
	return TRUE;
}

void
reset_view_history(struct view_history *history)
{
	while (pop_view_history_state(history, NULL, NULL))
		;
}

/*
 * Incremental updating
 */

void
reset_view(struct view *view)
{
	int i;

	for (i = 0; i < view->lines; i++)
		free(view->line[i].data);
	free(view->line);

	reset_matches(view);
	view->prev_pos = view->pos;
	/* A view without a previous view is the first view */
	if (!view->prev && !view->lines && view->prev_pos.lineno == 0)
		view->prev_pos.lineno = view->env->lineno;
	clear_position(&view->pos);

	if (view->columns)
		view_column_reset(view);

	view->line = NULL;
	view->lines  = 0;
	view->vid[0] = 0;
	view->custom_lines = 0;
	view->update_secs = 0;
}

static bool
restore_view_position(struct view *view)
{
	/* Ensure that the view position is in a valid state. */
	if (!check_position(&view->prev_pos) ||
	    (view->pipe && view->lines <= view->prev_pos.lineno))
		return goto_view_line(view, view->pos.offset, view->pos.lineno);

	/* Changing the view position cancels the restoring. */
	/* FIXME: Changing back to the first line is not detected. */
	if (check_position(&view->pos)) {
		clear_position(&view->prev_pos);
		return FALSE;
	}

	if (goto_view_line(view, view->prev_pos.offset, view->prev_pos.lineno) &&
	    view_is_displayed(view))
		werase(view->win);

	view->pos.col = view->prev_pos.col;
	clear_position(&view->prev_pos);

	return TRUE;
}

void
end_update(struct view *view, bool force)
{
	if (!view->pipe)
		return;
	while (!view->ops->read(view, NULL))
		if (!force)
			return;
	if (force)
		io_kill(view->pipe);
	io_done(view->pipe);
	view->pipe = NULL;
}

static void
setup_update(struct view *view, const char *vid)
{
	reset_view(view);
	/* XXX: Do not use string_copy_rev(), it copies until first space. */
	string_ncopy(view->vid, vid, strlen(vid));
	view->pipe = &view->io;
	view->start_time = time(NULL);
}

static bool
view_no_refresh(struct view *view, enum open_flags flags)
{
	bool reload = !!(flags & OPEN_ALWAYS_LOAD) || !view->lines;

	return (!reload && !strcmp(view->vid, view->ops->id)) ||
	       ((flags & OPEN_REFRESH) && !view_can_refresh(view));
}

bool
begin_update(struct view *view, const char *dir, const char **argv, enum open_flags flags)
{
	bool extra = !!(flags & (OPEN_EXTRA));
	bool refresh = flags & (OPEN_REFRESH | OPEN_PREPARED | OPEN_STDIN);
	enum io_flags forward_stdin = (flags & OPEN_FORWARD_STDIN) ? IO_RD_FORWARD_STDIN : 0;
	enum io_flags with_stderr = (flags & OPEN_WITH_STDERR) ? IO_RD_WITH_STDERR : 0;
	enum io_flags io_flags = forward_stdin | with_stderr;

	if (view_no_refresh(view, flags))
		return TRUE;

	if (view->pipe) {
		if (extra)
			io_done(view->pipe);
		else
			end_update(view, TRUE);
	}

	view->unrefreshable = open_in_pager_mode(flags);

	if (!refresh && argv) {
		bool file_filter = !view_has_flags(view, VIEW_FILE_FILTER) || opt_file_filter;

		view->dir = dir;
		if (!argv_format(view->env, &view->argv, argv, !view->prev, file_filter)) {
			report("Failed to format %s arguments", view->name);
			return FALSE;
		}

		/* Put the current view ref value to the view title ref
		 * member. This is needed by the blob view. Most other
		 * views sets it automatically after loading because the
		 * first line is a commit line. */
		string_copy_rev(view->ref, view->ops->id);
	}

	if (view->argv && view->argv[0] &&
	    !io_exec(&view->io, IO_RD, view->dir, opt_env, view->argv, io_flags)) {
		report("Failed to open %s view", view->name);
		return FALSE;
	}

	if (open_from_stdin(flags)) {
		if (!io_open(&view->io, "%s", ""))
			die("Failed to open stdin");
	}

	if (!extra)
		setup_update(view, view->ops->id);

	return TRUE;
}

bool
update_view(struct view *view)
{
	/* Clear the view and redraw everything since the tree sorting
	 * might have rearranged things. */
	bool redraw = view->lines == 0;
	bool can_read = TRUE;
	struct encoding *encoding = view->encoding ? view->encoding : default_encoding;
	struct buffer line;

	if (!view->pipe)
		return TRUE;

	if (!io_can_read(view->pipe, FALSE)) {
		if (view->lines == 0 && view_is_displayed(view)) {
			time_t secs = time(NULL) - view->start_time;

			if (secs > 1 && secs > view->update_secs) {
				if (view->update_secs == 0)
					redraw_view(view);
				update_view_title(view);
				view->update_secs = secs;
			}
		}
		return TRUE;
	}

	for (; io_get(view->pipe, &line, '\n', can_read); can_read = FALSE) {
		if (encoding && !encoding_convert(encoding, &line)) {
			report("Encoding failure");
			end_update(view, TRUE);
			return FALSE;
		}

		if (!view->ops->read(view, &line)) {
			report("Allocation failure");
			end_update(view, TRUE);
			return FALSE;
		}
	}

	if (io_error(view->pipe)) {
		report("Failed to read: %s", io_strerror(view->pipe));
		end_update(view, TRUE);

	} else if (io_eof(view->pipe)) {
		end_update(view, FALSE);
	}

	if (restore_view_position(view))
		redraw = TRUE;

	if (!view_is_displayed(view))
		return TRUE;

	if (redraw || view->force_redraw)
		redraw_view_from(view, 0);
	else
		redraw_view_dirty(view);
	view->force_redraw = FALSE;

	/* Update the title _after_ the redraw so that if the redraw picks up a
	 * commit reference in view->ref it'll be available here. */
	update_view_title(view);
	return TRUE;
}

void
update_view_title(struct view *view)
{
	WINDOW *window = view->title;
	struct line *line = &view->line[view->pos.lineno];
	unsigned int view_lines, lines;

	assert(view_is_displayed(view));

	if (view == display[current_view])
		wbkgdset(window, get_view_attr(view, LINE_TITLE_FOCUS));
	else
		wbkgdset(window, get_view_attr(view, LINE_TITLE_BLUR));

	werase(window);
	mvwprintw(window, 0, 0, "[%s]", view->name);

	if (*view->ref) {
		wprintw(window, " %s", view->ref);
	}

	if (!view_has_flags(view, VIEW_CUSTOM_STATUS) && view_has_line(view, line) &&
	    line->lineno) {
		wprintw(window, " - %s %d of %zd",
					   view->ops->type,
					   line->lineno,
					   view->lines - view->custom_lines);
	}

	if (view->pipe) {
		time_t secs = time(NULL) - view->start_time;

		/* Three git seconds are a long time ... */
		if (secs > 2)
			wprintw(window, " loading %lds", secs);
	}

	view_lines = view->pos.offset + view->height;
	lines = view->lines ? MIN(view_lines, view->lines) * 100 / view->lines : 0;
	mvwprintw(window, 0, view->width - count_digits(lines) - 1, "%d%%", lines);

	wnoutrefresh(window);
}

/*
 * View opening
 */

void
split_view(struct view *prev, struct view *view)
{
	display[1] = view;
	current_view = opt_focus_child ? 1 : 0;
	view->parent = prev;
	resize_display();

	if (prev->pos.lineno - prev->pos.offset >= prev->height) {
		/* Take the title line into account. */
		int lines = prev->pos.lineno - prev->pos.offset - prev->height + 1;

		/* Scroll the view that was split if the current line is
		 * outside the new limited view. */
		do_scroll_view(prev, lines);
	}

	if (view != prev && view_is_displayed(prev)) {
		/* "Blur" the previous view. */
		update_view_title(prev);
	}
}

void
maximize_view(struct view *view, bool redraw)
{
	memset(display, 0, sizeof(display));
	current_view = 0;
	display[current_view] = view;
	resize_display();
	if (redraw) {
		redraw_display(FALSE);
		report_clear();
	}
}

void
load_view(struct view *view, struct view *prev, enum open_flags flags)
{
	bool refresh = !view_no_refresh(view, flags);

	/* When prev == view it means this is the first loaded view. */
	if (prev && view != prev) {
		view->prev = prev;
	}

	if (!refresh && view_can_refresh(view) &&
	    watch_update_single(&view->watch, WATCH_EVENT_SWITCH_VIEW)) {
		refresh = watch_dirty(&view->watch);
		if (refresh)
			flags |= OPEN_REFRESH;
	}

	if (refresh) {
		if (view->pipe)
			end_update(view, TRUE);
		if (view->ops->private_size) {
			if (!view->private) {
				view->private = calloc(1, view->ops->private_size);
			} else {
				if (view->ops->done)
					view->ops->done(view);
				memset(view->private, 0, view->ops->private_size);
			}
		}

		if (!view->ops->open(view, flags))
			return;
	}

	if (prev) {
		bool split = !!(flags & OPEN_SPLIT);

		if (split) {
			split_view(prev, view);
		} else {
			maximize_view(view, FALSE);
		}
	}

	restore_view_position(view);

	if (view->pipe && view->lines == 0) {
		/* Clear the old view and let the incremental updating refill
		 * the screen. */
		werase(view->win);
		/* Do not clear the position if it is the first view. */
		if (view->prev && !(flags & (OPEN_RELOAD | OPEN_REFRESH)))
			clear_position(&view->prev_pos);
		report_clear();
	} else if (view_is_displayed(view)) {
		redraw_view(view);
		report_clear();
	}
}

#define refresh_view(view) load_view(view, NULL, OPEN_REFRESH)
#define reload_view(view) load_view(view, NULL, OPEN_RELOAD)

void
open_view(struct view *prev, struct view *view, enum open_flags flags)
{
	bool reload = !!(flags & (OPEN_RELOAD | OPEN_PREPARED));
	int nviews = displayed_views();

	assert(flags ^ OPEN_REFRESH);

	if (view == prev && nviews == 1 && !reload) {
		report("Already in %s view", view->name);
		return;
	}

	if (!view_has_flags(view, VIEW_NO_GIT_DIR) && !repo.git_dir[0]) {
		report("The %s view is disabled in pager mode", view->name);
		return;
	}

	if (!view->keymap)
		view->keymap = get_keymap(view->name, strlen(view->name));
	load_view(view, prev ? prev : view, flags);
}

void
open_argv(struct view *prev, struct view *view, const char *argv[], const char *dir, enum open_flags flags)
{
	if (view->pipe)
		end_update(view, TRUE);
	view->dir = dir;

	if (!argv_copy(&view->argv, argv)) {
		report("Failed to open %s view: %s", view->name, io_strerror(&view->io));
	} else {
		open_view(prev, view, flags | OPEN_PREPARED);
	}
}

/*
 * Various utilities.
 */

static struct view *sorting_view;

#define apply_comparator(cmp, o1, o2) \
	(!(o1) || !(o2)) ? !!(o2) - !!(o1) : cmp(o1, o2)

#define number_compare(size1, size2)	(*(size1) - *(size2))

#define mode_is_dir(mode)		((mode) && S_ISDIR(*(mode)))

static int
compare_view_column(enum view_column_type column, bool use_file_mode,
		    const struct line *line1, struct view_column_data *column_data1,
		    const struct line *line2, struct view_column_data *column_data2)
{
	switch (column) {
	case VIEW_COLUMN_AUTHOR:
		return apply_comparator(ident_compare, column_data1->author, column_data2->author);

	case VIEW_COLUMN_DATE:
		return apply_comparator(timecmp, column_data1->date, column_data2->date);

	case VIEW_COLUMN_ID:
		if (column_data1->reflog && column_data2->reflog)
			return apply_comparator(strcmp, column_data1->reflog, column_data2->reflog);
		return apply_comparator(strcmp, column_data1->id, column_data2->id);

	case VIEW_COLUMN_FILE_NAME:
		if (use_file_mode && mode_is_dir(column_data1->mode) != mode_is_dir(column_data2->mode))
			return mode_is_dir(column_data1->mode) ? -1 : 1;
		return apply_comparator(strcmp, column_data1->file_name, column_data2->file_name);

	case VIEW_COLUMN_FILE_SIZE:
		return apply_comparator(number_compare, column_data1->file_size, column_data2->file_size);

	case VIEW_COLUMN_LINE_NUMBER:
		return line1->lineno - line2->lineno;

	case VIEW_COLUMN_MODE:
		return apply_comparator(number_compare, column_data1->mode, column_data2->mode);

	case VIEW_COLUMN_REF:
		return apply_comparator(ref_compare, column_data1->ref, column_data2->ref);

	case VIEW_COLUMN_COMMIT_TITLE:
		return apply_comparator(strcmp, column_data1->commit_title, column_data2->commit_title);

	case VIEW_COLUMN_SECTION:
		return apply_comparator(strcmp, column_data1->section->opt.section.text,
						column_data2->section->opt.section.text);

	case VIEW_COLUMN_STATUS:
		return apply_comparator(number_compare, column_data1->status, column_data2->status);

	case VIEW_COLUMN_TEXT:
		return apply_comparator(strcmp, column_data1->text, column_data2->text);
	}

	return 0;
}

static enum view_column_type view_column_order[] = {
	VIEW_COLUMN_FILE_NAME,
	VIEW_COLUMN_STATUS,
	VIEW_COLUMN_MODE,
	VIEW_COLUMN_FILE_SIZE,
	VIEW_COLUMN_DATE,
	VIEW_COLUMN_AUTHOR,
	VIEW_COLUMN_COMMIT_TITLE,
	VIEW_COLUMN_LINE_NUMBER,
	VIEW_COLUMN_SECTION,
	VIEW_COLUMN_TEXT,
	VIEW_COLUMN_REF,
	VIEW_COLUMN_ID,
};

static int
sort_view_compare(const void *l1, const void *l2)
{
	const struct line *line1 = l1;
	const struct line *line2 = l2;
	struct view_column_data column_data1 = {};
	struct view_column_data column_data2 = {};
	struct sort_state *sort = &sorting_view->sort;
	enum view_column_type column = get_sort_field(sorting_view);
	int cmp;
	int i;

	if (!sorting_view->ops->get_column_data(sorting_view, line1, &column_data1))
		return -1;
	else if (!sorting_view->ops->get_column_data(sorting_view, line2, &column_data2))
		return 1;

	cmp = compare_view_column(column, TRUE, line1, &column_data1, line2, &column_data2);

	/* Ensure stable sorting by ordering ordering by the other
	 * columns if the selected column values are equal. */
	for (i = 0; !cmp && i < ARRAY_SIZE(view_column_order); i++)
		if (column != view_column_order[i])
			cmp = compare_view_column(view_column_order[i], FALSE,
						  line1, &column_data1,
						  line2, &column_data2);

	return sort->reverse ? -cmp : cmp;
}

void
sort_view(struct view *view, bool change_field)
{
	struct sort_state *state = &view->sort;

	if (change_field) {
		while (TRUE) {
			state->current = state->current->next
				? state->current->next : view->columns;
			if (get_sort_field(view) == VIEW_COLUMN_ID &&
			    !state->current->opt.id.display)
				continue;
			break;
		}
	} else {
		state->reverse = !state->reverse;
	}

	sorting_view = view;
	qsort(view->line, view->lines, sizeof(*view->line), sort_view_compare);
}

static const char *
view_column_text(struct view *view, struct view_column_data *column_data,
		 struct view_column *column)
{
	const char *text = "";

	switch (column->type) {
	case VIEW_COLUMN_AUTHOR:
		if (column_data->author)
			text = mkauthor(column_data->author, column->opt.author.width, column->opt.author.display);
		break;

	case VIEW_COLUMN_COMMIT_TITLE:
		text = column_data->commit_title;
		break;

	case VIEW_COLUMN_DATE:
		if (column_data->date)
			text = mkdate(column_data->date, column->opt.date.display);
		break;

	case VIEW_COLUMN_REF:
		if (column_data->ref)
			text = column_data->ref->name;
		break;

	case VIEW_COLUMN_FILE_NAME:
		if (column_data->file_name)
			text = column_data->file_name;
		break;

	case VIEW_COLUMN_FILE_SIZE:
		if (column_data->file_size)
			text = mkfilesize(*column_data->file_size, column->opt.file_size.display);
		break;

	case VIEW_COLUMN_ID:
		if (column->opt.id.display)
			text = column_data->reflog ? column_data->reflog : column_data->id;
		break;

	case VIEW_COLUMN_LINE_NUMBER:
		break;

	case VIEW_COLUMN_MODE:
		if (column_data->mode)
			text = mkmode(*column_data->mode);
		break;

	case VIEW_COLUMN_STATUS:
		if (column_data->status)
			text = mkstatus(*column_data->status, column->opt.status.display);
		break;

	case VIEW_COLUMN_SECTION:
		text = column_data->section->opt.section.text;
		break;

	case VIEW_COLUMN_TEXT:
		text = column_data->text;
		break;
	}

	return text ? text : "";
}

static bool
grep_refs(struct view *view, struct view_column *column, const struct ref_list *list)
{
	regmatch_t pmatch;
	size_t i;

	if (!list)
		return FALSE;

	for (i = 0; i < list->size; i++) {
		if (!regexec(view->regex, list->refs[i]->name, 1, &pmatch, 0))
			return TRUE;
	}

	return FALSE;
}

bool
view_column_grep(struct view *view, struct line *line)
{
	struct view_column_data column_data = {};
	bool ok = view->ops->get_column_data(view, line, &column_data);
	struct view_column *column;

	if (!ok)
		return FALSE;

	for (column = view->columns; column; column = column->next) {
		const char *text[] = {
			view_column_text(view, &column_data, column),
			NULL
		};

		if (grep_text(view, text))
			return TRUE;

		if (column->type == VIEW_COLUMN_COMMIT_TITLE &&
		    column->opt.commit_title.refs &&
		    grep_refs(view, column, column_data.refs))
			return TRUE;
	}

	return FALSE;
}

bool
view_column_info_changed(struct view *view, bool update)
{
	struct view_column *column;
	bool changed = FALSE;

	for (column = view->columns; column; column = column->next) {
		if (memcmp(&column->prev_opt, &column->opt, sizeof(column->opt))) {
			if (!update)
				return TRUE;
			column->prev_opt = column->opt;
			changed = TRUE;
		}
	}

	return changed;
}

void
view_column_reset(struct view *view)
{
	struct view_column *column;

	view_column_info_changed(view, TRUE);
	for (column = view->columns; column; column = column->next)
		column->width = 0;
}

static enum status_code
parse_view_column_config(char **pos, const char **name, const char **value, bool first)
{
	size_t len = strcspn(*pos, ",");
	size_t optlen;

	if (strlen(*pos) > len)
		(*pos)[len] = 0;
	optlen = strcspn(*pos, ":=");

	if (first) {
		*name = "display";

		if (optlen == len) {
			*value = len ? *pos : "yes";
			*pos += len + 1;
			return SUCCESS;
		}

		/* Fake boolean enum value. */
		*value = "yes";
		return SUCCESS;
	}

	*name = *pos;
	if (optlen == len)
		*value = "yes";
	else
		*value = *pos + optlen + 1;
	(*pos)[optlen] = 0;
	*pos += len + 1;

	return SUCCESS;
}

static enum status_code
parse_view_column_option(struct view_column *column,
			 const char *opt_name, const char *opt_value)
{
#define DEFINE_COLUMN_OPTION_INFO(name, type, flags) \
	{ #name, STRING_SIZE(#name), #type, &opt->name },

#define DEFINE_COLUMN_OPTIONS_PARSE(name, id, options) \
	if (column->type == VIEW_COLUMN_##id) { \
		struct name##_options *opt = &column->opt.name; \
		struct option_info info[] = { \
			options(DEFINE_COLUMN_OPTION_INFO) \
		}; \
		struct option_info *option = find_option_info(info, ARRAY_SIZE(info), opt_name); \
		if (!option) \
			return error("Unknown option `%s' for column %s", opt_name, \
				     view_column_name(VIEW_COLUMN_##id)); \
		return parse_option(option, #name, opt_value); \
	}

	COLUMN_OPTIONS(DEFINE_COLUMN_OPTIONS_PARSE);

	return error("Unknown view column option: %s", opt_name);
}

static enum status_code
parse_view_column_type(struct view_column *column, const char **arg)
{
	enum view_column_type type;
	size_t typelen = strcspn(*arg, ":,");

	for (type = 0; type < view_column_type_map->size; type++)
		if (enum_equals(view_column_type_map->entries[type], *arg, typelen)) {
			*arg += typelen + !!(*arg)[typelen];
			column->type = type;
			return SUCCESS;
		}

	return error("Failed to parse view column type: %.*s", (int) typelen, *arg);
}

static struct view *
find_view(const char *view_name)
{
	struct view *view;
	int i;

	foreach_view(view, i)
		if (!strncmp(view_name, view->name, strlen(view->name)))
			return view;

	return NULL;
}

enum status_code
parse_view_config(const char *view_name, const char *argv[])
{
	enum status_code code = SUCCESS;
	size_t size = argv_size(argv);
	struct view_column *columns;
	struct view_column *column;
	struct view *view = find_view(view_name);
	int i;

	if (!view)
		return error("Unknown view: %s", view_name);

	columns = calloc(size, sizeof(*columns));
	if (!columns)
		return ERROR_OUT_OF_MEMORY;

	for (i = 0, column = NULL; code == SUCCESS && i < size; i++) {
		const char *arg = argv[i];
		char buf[SIZEOF_STR] = "";
		char *pos, *end;
		bool first = TRUE;

		if (column)
			column->next = &columns[i];
		column = &columns[i];

		code = parse_view_column_type(column, &arg);
		if (code != SUCCESS)
			break;

		if (!(view->ops->column_bits & (1 << column->type)))
			return error("The %s view does not support %s column", view->name,
				     view_column_name(column->type));

		if ((column->type == VIEW_COLUMN_TEXT ||
		     column->type == VIEW_COLUMN_COMMIT_TITLE) &&
		     i + 1 < size)
			return error("The %s column must always be last",
				     view_column_name(column->type));

		string_ncopy(buf, arg, strlen(arg));

		for (pos = buf, end = pos + strlen(pos); code == SUCCESS && pos <= end; first = FALSE) {
			const char *name = NULL;
			const char *value = NULL;

			code = parse_view_column_config(&pos, &name, &value, first);
			if (code == SUCCESS)
				code = parse_view_column_option(column, name, value);
		}

		column->prev_opt = column->opt;
	}

	if (code == SUCCESS) {
		free(view->columns);
		view->columns = columns;
		view->sort.current = view->columns;
	} else {
		free(columns);
	}

	return code;
}

struct view_column *
get_view_column(struct view *view, enum view_column_type type)
{
	struct view_column *column;

	for (column = view->columns; column; column = column->next)
		if (column->type == type)
			return column;
	return NULL;
}

bool
view_column_info_update(struct view *view, struct line *line)
{
	struct view_column_data column_data = {};
	struct view_column *column;
	bool changed = FALSE;

	if (!view->ops->get_column_data(view, line, &column_data))
		return FALSE;

	for (column = view->columns; column; column = column->next) {
		const char *text = view_column_text(view, &column_data, column);
		int width = 0;

		switch (column->type) {
		case VIEW_COLUMN_AUTHOR:
			width = column->opt.author.width;
			break;

		case VIEW_COLUMN_COMMIT_TITLE:
			width = column->opt.commit_title.width;
			break;

		case VIEW_COLUMN_DATE:
			width = column->opt.date.width;
			break;

		case VIEW_COLUMN_FILE_NAME:
			width = column->opt.file_name.width;
			break;

		case VIEW_COLUMN_FILE_SIZE:
			width = column->opt.file_size.width;
			break;

		case VIEW_COLUMN_ID:
			width = column->opt.id.width;
			if (!width)
				width = opt_id_width;
			if (!column_data.reflog && !width)
				width = 7;
			break;

		case VIEW_COLUMN_LINE_NUMBER:
			if (column_data.line_number)
				width = count_digits(*column_data.line_number);
			else
				width = count_digits(view->lines);
			if (width < 3)
				width = 3;
			break;

		case VIEW_COLUMN_MODE:
			width = column->opt.mode.width;
			break;

		case VIEW_COLUMN_REF:
			width = column->opt.ref.width;
			break;

		case VIEW_COLUMN_SECTION:
			break;

		case VIEW_COLUMN_STATUS:
			width = column->opt.status.width;
			break;

		case VIEW_COLUMN_TEXT:
			width = column->opt.text.width;
			break;
		}

		if (*text && !width)
			width = utf8_width(text);

		if (width > column->width) {
			column->width = width;
			changed = TRUE;
		}
	}

	if (changed)
		view->force_redraw = TRUE;
	return changed;
}

struct line *
find_line_by_type(struct view *view, struct line *line, enum line_type type, int direction)
{
	for (; view_has_line(view, line); line += direction)
		if (line->type == type)
			return line;

	return NULL;
}

/*
 * Line utilities.
 */

DEFINE_ALLOCATOR(realloc_lines, struct line, 256)

struct line *
add_line_at(struct view *view, unsigned long pos, const void *data, enum line_type type, size_t data_size, bool custom)
{
	struct line *line;
	unsigned long lineno;

	if (!realloc_lines(&view->line, view->lines, 1))
		return NULL;

	if (data_size) {
		void *alloc_data = calloc(1, data_size);

		if (!alloc_data)
			return NULL;

		if (data)
			memcpy(alloc_data, data, data_size);
		data = alloc_data;
	}

	if (pos < view->lines) {
		view->lines++;
		line = view->line + pos;
		lineno = line->lineno;

		memmove(line + 1, line, (view->lines - pos) * sizeof(*view->line));
		while (pos < view->lines) {
			view->line[pos].lineno++;
			view->line[pos++].dirty = 1;
		}
	} else {
		line = &view->line[view->lines++];
		lineno = view->lines - view->custom_lines;
	}

	memset(line, 0, sizeof(*line));
	line->type = type;
	line->data = (void *) data;
	line->dirty = 1;

	if (custom)
		view->custom_lines++;
	else
		line->lineno = lineno;

	return line;
}

struct line *
add_line(struct view *view, const void *data, enum line_type type, size_t data_size, bool custom)
{
	return add_line_at(view, view->lines, data, type, data_size, custom);
}

struct line *
add_line_alloc_(struct view *view, void **ptr, enum line_type type, size_t data_size, bool custom)
{
	struct line *line = add_line(view, NULL, type, data_size, custom);

	if (line)
		*ptr = line->data;
	return line;
}

struct line *
add_line_nodata(struct view *view, enum line_type type)
{
	return add_line(view, NULL, type, 0, FALSE);
}

struct line *
add_line_text(struct view *view, const char *text, enum line_type type)
{
	struct line *line = add_line(view, text, type, strlen(text) + 1, FALSE);

	if (line && view->ops->column_bits)
		view_column_info_update(view, line);
	return line;
}

struct line * PRINTF_LIKE(3, 4)
add_line_format(struct view *view, enum line_type type, const char *fmt, ...)
{
	char buf[SIZEOF_STR];
	int retval;

	FORMAT_BUFFER(buf, sizeof(buf), fmt, retval, FALSE);
	return retval >= 0 ? add_line_text(view, buf, type) : NULL;
}

/*
 * Global view state.
 */

/* Included last to not pollute the rest of the file. */
#include "tig/main.h"
#include "tig/diff.h"
#include "tig/log.h"
#include "tig/tree.h"
#include "tig/blob.h"
#include "tig/blame.h"
#include "tig/refs.h"
#include "tig/status.h"
#include "tig/stage.h"
#include "tig/stash.h"
#include "tig/grep.h"
#include "tig/pager.h"
#include "tig/help.h"

static struct view *views[] = {
#define VIEW_DATA(id, name) &name##_view
	VIEW_INFO(VIEW_DATA)
};

struct view *
get_view(int i)
{
	return 0 <= i && i < ARRAY_SIZE(views) ? views[i] : NULL;
}

/* vim: set ts=8 sw=8 noexpandtab: */
