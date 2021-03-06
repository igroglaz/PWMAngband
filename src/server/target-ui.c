/*
 * File: target-ui.c
 * Purpose: UI for targeting code
 *
 * Copyright (c) 1997-2014 Angband contributors
 * Copyright (c) 2021 MAngband and PWMAngband Developers
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */


#include "s-angband.h"


/*
 * Display targeting help at the bottom of the screen
 */
void target_display_help(char *help, size_t len, bool monster, bool free)
{
    /* Display help */
    my_strcpy(help, "[Press <dir>, 'p', 'q', 'r'", len);
    if (free) my_strcat(help, ", 'm'", len);
    else my_strcat(help, ", '+', '-', 'o'", len);
    if (monster || free) my_strcat(help, ", 't'", len);
    my_strcat(help, ", Return, or Space]", len);
}


/*
 * Perform the minimum "whole panel" adjustment to ensure that the given
 * location is contained inside the current panel, and return true if any
 * such adjustment was performed.
 */
static bool adjust_panel_help(struct player *p, int y, int x)
{
    struct loc grid;
    int screen_hgt, screen_wid;
    int panel_wid, panel_hgt;

    screen_hgt = p->screen_rows / p->tile_hgt;
    screen_wid = p->screen_cols / p->tile_wid;

    panel_wid = screen_wid / 2;
    panel_hgt = screen_hgt / 2;

    /* Paranoia */
    if (panel_wid < 1) panel_wid = 1;
    if (panel_hgt < 1) panel_hgt = 1;

    loc_copy(&grid, &p->offset_grid);

    /* Adjust as needed */
    while (y >= grid.y + screen_hgt) grid.y += panel_hgt;
    while (y < grid.y) grid.y -= panel_hgt;

    /* Adjust as needed */
    while (x >= grid.x + screen_wid) grid.x += panel_wid;
    while (x < grid.x) grid.x -= panel_wid;

    /* Use "modify_panel" */
    return (modify_panel(p, &grid));
}


/*
 * Do we need to inform client about target info?
 */
static bool need_target_info(struct player *p, u32b query, byte step)
{
    bool need_info = false;

    /* Acknowledge */
    if (query == '\0') need_info = true;

    /* Next step */
    if (p->tt_step < step) need_info = true;

    /* Print help */
    if ((query == KC_ENTER) && (p->tt_step == step) && p->tt_help)
        need_info = true;

    /* Advance step */
    if (need_info) p->tt_step = step;

    /* Clear help */
    else p->tt_help = false;

    return need_info;
}


/*
 * Inform client about target info
 */
static bool target_info(struct player *p, struct loc *grid, const char *info, const char *help,
    u32b query)
{
    int col = grid->x - p->offset_grid.x;
    int row = grid->y - p->offset_grid.y + 1;
    bool dble = true;
    struct loc above;

    next_grid(&above, grid, DIR_N);

    /* Do nothing on quit */
    if ((query == 'q') || (query == ESCAPE)) return false;

    /* Hack -- is there something targetable above our position? */
    if (square_in_bounds_fully(chunk_get(&p->wpos), &above) && target_accept(p, &above))
        dble = false;

    /* Display help info */
    if (p->tt_help)
        Send_target_info(p, col, row, dble, help);

    /* Display target info */
    else
        Send_target_info(p, col, row, dble, info);

    /* Toggle help */
    p->tt_help = !p->tt_help;

    return true;
}


/*
 * Examine a grid, return a keypress.
 *
 * The "mode" argument contains the "TARGET_LOOK" bit flag, which
 * indicates that the "space" key should scan through the contents
 * of the grid, instead of simply returning immediately.  This lets
 * the "look" command get complete information, without making the
 * "target" command annoying.
 *
 * The "info" argument contains the "commands" which should be shown
 * inside the "[xxx]" text.  This string must never be empty, or grids
 * containing monsters will be displayed with an extra comma.
 *
 * Note that if a monster is in the grid, we update both the monster
 * recall info and the health bar info to track that monster.
 *
 * This function correctly handles multiple objects per grid, and objects
 * and terrain features in the same grid, though the latter never happens.
 *
 * This function must handle blindness/hallucination.
 */
static bool target_set_interactive_aux(struct player *p, struct loc *grid, int mode,
    const char *help, u32b query)
{
    const char *s1, *s2, *s3;
    bool boring;
    int feat;
    int floor_max = z_info->floor_size;
    struct object **floor_list = mem_zalloc(floor_max * sizeof(*floor_list));
    int floor_num;
    char out_val[256];
    char coords[20];
    int tries = 200;
    struct chunk *c = chunk_get(&p->wpos);
    struct source who_body;
    struct source *who = &who_body;

    square_actor(c, grid, who);

    /* Describe the square location */
    grid_desc(p, coords, sizeof(coords), grid);

    /* Repeat forever */
    while (tries--)
    {
        struct trap *trap;

        /* Assume boring */
        boring = true;

        /* Default */
        s1 = "You see ";
        s2 = "";
        s3 = "";

        /* Bail if looking at a forbidden grid */
        if (!square_in_bounds(c, grid)) break;

        /* The player */
        if (who->player && (who->player == p))
        {
            /* Description */
            s1 = "You are ";

            /* Preposition */
            s2 = "on ";
        }

        /* Hallucination messes things up */
        if (p->timed[TMD_IMAGE])
        {
            const char *name = "something strange";

            /* Display a message */
            strnfmt(out_val, sizeof(out_val), "%s%s%s%s, %s.", s1, s2, s3, name, coords);

            /* Inform client */
            if (need_target_info(p, query, TARGET_NONE))
            {
                mem_free(floor_list);
                return target_info(p, grid, out_val, help, query);
            }

            /* Stop on everything but "return" */
            if (query == KC_ENTER)
            {
                query = '\0';
                continue;
            }

            mem_free(floor_list);
            return false;
        }

        /* Actual players */
        if (who->player && (who->player != p))
        {
            /* Visible */
            if (player_is_visible(p, who->idx))
            {
                bool recall = false;
                char player_name[NORMAL_WID];

                /* Not boring */
                boring = false;

                /* Unaware players get a pseudo description */
                if (who->player->k_idx)
                {
                    /* Acting as an object: get a pseudo object description */
                    if (who->player->k_idx > 0)
                    {
                        struct object_kind *kind = &k_info[who->player->k_idx];
                        struct object *fake = object_new();

                        object_prep(p, c, fake, kind, 0, MINIMISE);
                        if (tval_is_money_k(kind)) fake->pval = 1;
                        object_desc(p, player_name, sizeof(player_name), fake,
                            ODESC_PREFIX | ODESC_BASE);
                        object_delete(&fake);
                    }

                    /* Acting as a feature: get a pseudo feature description */
                    else
                    {
                        feat = feat_pseudo(who->player->poly_race->d_char);
                        my_strcpy(player_name, f_info[feat].name, sizeof(player_name));
                        s3 = (is_a_vowel(player_name[0])? "an ": "a ");
                    }

                    /* Describe the player */
                    strnfmt(out_val, sizeof(out_val), "%s%s%s%s, %s.", s1, s2, s3, player_name,
                        coords);

                    /* Inform client */
                    if (need_target_info(p, query, TARGET_MON))
                    {
                        mem_free(floor_list);
                        return target_info(p, grid, out_val, help, query);
                    }

                    /* Stop on everything but "return" */
                    if (query != KC_ENTER) break;

                    /* Paranoia */
                    mem_free(floor_list);
                    return true;
                }

                /* Get the player name */
                strnfmt(player_name, sizeof(player_name), "%s the %s %s", who->player->name,
                    who->player->race->name, who->player->clazz->name);

                /* Hack -- track this player */
                monster_race_track(p->upkeep, who);

                /* Hack -- health bar for this player */
                health_track(p->upkeep, who);

                /* Hack -- track cursor for this player */
                cursor_track(p, who);

                /* Hack -- handle stuff */
                handle_stuff(p);

                /* Interact */
                if ((query == 'r') && (p->tt_step == TARGET_MON))
                    recall = true;

                /* Recall or target */
                if (recall)
                {
                    /* Recall on screen */
                    do_cmd_describe(p);
                    mem_free(floor_list);
                    return false;
                }
                else
                {
                    char buf[NORMAL_WID];

                    /* Describe the player */
                    look_player_desc(who->player, buf, sizeof(buf));

                    /* Describe, and prompt for recall */
                    strnfmt(out_val, sizeof(out_val), "%s%s%s%s (%s), %s.",
                        s1, s2, s3, player_name, buf, coords);

                    /* Inform client */
                    if (need_target_info(p, query, TARGET_MON))
                    {
                        mem_free(floor_list);
                        return target_info(p, grid, out_val, help, query);
                    }
                }

                /* Stop on everything but "return"/"space" */
                if ((query != KC_ENTER) && (query != ' ')) break;

                /* Sometimes stop at "space" key */
                if ((query == ' ') && !(mode & (TARGET_LOOK))) break;

                /* Take account of gender */
                if (who->player->psex == SEX_FEMALE) s1 = "She is ";
                else if (who->player->psex == SEX_MALE) s1 = "He is ";
                else s1 = "It is ";

                /* Use a preposition */
                s2 = "on ";
            }
        }

        /* Actual monsters */
        if (who->monster)
        {
            /* Visible */
            if (monster_is_obvious(p, who->idx, who->monster))
            {
                bool recall = false;
                char m_name[NORMAL_WID];

                /* Not boring */
                boring = false;

                /* Get the monster name ("a kobold") */
                monster_desc(p, m_name, sizeof(m_name), who->monster, MDESC_IND_VIS);

                /* Hack -- track this monster race */
                monster_race_track(p->upkeep, who);

                /* Hack -- health bar for this monster */
                health_track(p->upkeep, who);

                /* Hack -- track cursor for this monster */
                cursor_track(p, who);

                /* Hack -- handle stuff */
                handle_stuff(p);

                /* Interact */
                if ((query == 'r') && (p->tt_step == TARGET_MON))
                    recall = true;

                /* Recall */
                if (recall)
                {
                    /* Recall on screen */
                    do_cmd_describe(p);
                    mem_free(floor_list);
                    return false;
                }

                /* Normal */
                else
                {
                    char buf[NORMAL_WID];

                    /* Describe the monster */
                    look_mon_desc(who->monster, buf, sizeof(buf));

                    /* Describe, and prompt for recall */
                    strnfmt(out_val, sizeof(out_val), "%s%s%s%s (%s), %s.", s1, s2, s3, m_name, buf,
                        coords);

                    /* Inform client */
                    if (need_target_info(p, query, TARGET_MON))
                    {
                        mem_free(floor_list);
                        return target_info(p, grid, out_val, help, query);
                    }
                }

                /* Stop on everything but "return"/"space" */
                if ((query != KC_ENTER) && (query != ' ')) break;

                /* Sometimes stop at "space" key */
                if ((query == ' ') && !(mode & (TARGET_LOOK))) break;

                /* Take account of gender */
                if (rf_has(who->monster->race->flags, RF_FEMALE)) s1 = "She is ";
                else if (rf_has(who->monster->race->flags, RF_MALE)) s1 = "He is ";
                else s1 = "It is ";

                /* Describe carried objects (DMs only) */
                if (is_dm_p(p))
                {
                    /* Use a verb */
                    s2 = "carrying ";

                    /* Change the intro */
                    if (p->tt_o) s2 = "also carrying ";

                    /* Scan all objects being carried */
                    if (!p->tt_o) p->tt_o = who->monster->held_obj;
                    else p->tt_o = p->tt_o->next;
                    if (p->tt_o)
                    {
                        char o_name[NORMAL_WID];

                        /* Obtain an object description */
                        object_desc(p, o_name, sizeof(o_name), p->tt_o, ODESC_PREFIX | ODESC_FULL);

                        /* Describe the object */
                        strnfmt(out_val, sizeof(out_val), "%s%s%s%s, %s.", s1, s2, s3, o_name,
                            coords);

                        /* Inform client */
                        mem_free(floor_list);
                        return target_info(p, grid, out_val, help, query);
                    }
                }

                /* Use a preposition */
                s2 = "on ";
            }
        }

        /* A trap */
        trap = square_known_trap(p, c, grid);
		if (trap)
		{
			bool recall = false;

			/* Not boring */
			boring = false;

            /* Pick proper indefinite article */
            s3 = (is_a_vowel(trap->kind->desc[0])? "an ": "a ");

            /* Interact */
            if ((query == 'r') && (p->tt_step == TARGET_TRAP))
                recall = true;

            /* Recall */
            if (recall)
            {
                /* Recall on screen */
                describe_trap(p, trap);
                mem_free(floor_list);
                return false;
            }

            /* Normal */
            else
            {
                /* Describe, and prompt for recall */
                strnfmt(out_val, sizeof(out_val), "%s%s%s%s, %s.", s1, s2, s3, trap->kind->desc,
                    coords);

                /* Inform client */
                if (need_target_info(p, query, TARGET_TRAP))
                {
                    mem_free(floor_list);
                    return target_info(p, grid, out_val, help, query);
                }
            }

            /* Stop on everything but "return"/"space" */
            if ((query != KC_ENTER) && (query != ' ')) break;

            /* Sometimes stop at "space" key */
            if ((query == ' ') && !(mode & (TARGET_LOOK))) break;
		}

		/* Double break */
		if (square_known_trap(p, c, grid)) break;

        /* Scan all sensed objects in the grid */
        floor_num = scan_distant_floor(p, c, floor_list, floor_max, grid);
        if (floor_num > 0)
        {
            /* Not boring */
            boring = false;

            track_object(p->upkeep, floor_list[0]);
            handle_stuff(p);

            /* If there is more than one item... */
            if (floor_num > 1)
            {
                /* Describe the pile */
                strnfmt(out_val, sizeof(out_val), "%s%s%sa pile of %d objects, %s.",
                    s1, s2, s3, floor_num, coords);

                /* Inform client */
                if (need_target_info(p, query, TARGET_OBJ))
                {
                    mem_free(floor_list);
                    return target_info(p, grid, out_val, help, query);
                }

                /* Display objects */
                if (query == 'r')
                {
                    msg(p, "You see:");
                    display_floor(p, c, floor_list, floor_num, false);
                    show_floor(p, OLIST_WEIGHT | OLIST_GOLD);
                    mem_free(floor_list);
                    return false;
                }

                /* Done */
                break;
            }

            /* Only one object to display */
            else
            {
                bool recall = false;
                char o_name[NORMAL_WID];

                /* Get the single object in the list */
                struct object *obj = floor_list[0];

                /* Not boring */
                boring = false;

                /* Obtain an object description */
                object_desc(p, o_name, sizeof(o_name), obj, ODESC_PREFIX | ODESC_FULL);

                /* Interact */
                if ((query == 'r') && (p->tt_step == TARGET_OBJ))
                    recall = true;

                /* Recall */
                if (recall)
                {
                    /* Recall on screen */
                    display_object_recall_interactive(p, obj, o_name);
                    mem_free(floor_list);
                    return false;
                }

                /* Normal */
                else
                {
                    /* Describe, and prompt for recall */
                    strnfmt(out_val, sizeof(out_val), "%s%s%s%s, %s.", s1, s2, s3, o_name, coords);

                    /* Inform client */
                    if (need_target_info(p, query, TARGET_OBJ))
                    {
                        mem_free(floor_list);
                        return target_info(p, grid, out_val, help, query);
                    }
                }

                /* Stop on everything but "return"/"space" */
                if ((query != KC_ENTER) && (query != ' ')) break;

                /* Sometimes stop at "space" key */
                if ((query == ' ') && !(mode & (TARGET_LOOK))) break;

                /* Plurals */
                s1 = VERB_AGREEMENT(obj->number, "It is ", "They are ");

                /* Preposition */
                s2 = "on ";
            }
        }

        feat = square_apparent_feat(p, c, grid);

        /* Terrain feature if needed */
        if (boring || feat_isterrain(feat))
        {
            const char *name = square_apparent_name(p, c, grid);
            bool recall = false;
            struct location *dungeon = get_dungeon(&p->wpos);

            /* Pick a preposition if needed */
            if (*s2) s2 = square_apparent_look_in_preposition(p, c, grid);

            /* Pick prefix for the name */
            s3 = square_apparent_look_prefix(p, c, grid);

            /* Hack -- dungeon entrance */
            if (dungeon && square_isdownstairs(c, grid))
            {
                s3 = "the entrance to ";
                name = dungeon->name;
            }

            /* Interact */
            if ((query == 'r') && (p->tt_step == TARGET_FEAT))
                recall = true;

            /* Recall */
            if (recall)
            {
                /* Recall on screen */
                describe_feat(p, &f_info[feat]);
                mem_free(floor_list);
                return false;
            }

            /* Normal */
            else
            {
                /* Message */
                strnfmt(out_val, sizeof(out_val), "%s%s%s%s, %s.", s1, s2, s3, name, coords);

                /* Inform client */
                if (need_target_info(p, query, TARGET_FEAT))
                {
                    mem_free(floor_list);
                    return target_info(p, grid, out_val, help, query);
                }
            }

            /* Stop on everything but "return"/"space" */
            if ((query != KC_ENTER) && (query != ' ')) break;
        }

        /* Stop on everything but "return" */
        if (query != KC_ENTER) break;

        /* Paranoia */
        mem_free(floor_list);
        return true;
    }

    /* Paranoia */
    if (!tries)
        plog_fmt("Infinite loop in target_set_interactive_aux: %c", query);

    mem_free(floor_list);

    /* Keep going */
    return false;
}


/*
 * Draw a visible path over the squares between (x1,y1) and (x2,y2).
 *
 * The path consists of "*", which are white except where there is a
 * monster, object or feature in the grid.
 *
 * This routine has (at least) three weaknesses:
 * - remembered objects/walls which are no longer present are not shown,
 * - squares which (e.g.) the player has walked through in the dark are
 *   treated as unknown space.
 * - walls which appear strange due to hallucination aren't treated correctly.
 *
 * The first two result from information being lost from the dungeon arrays,
 * which requires changes elsewhere.
 */
int draw_path(struct player *p, u16b path_n, struct loc *path_g, struct loc *grid)
{
    int i;
    bool on_screen;
    bool pastknown = false;
    struct chunk *c = chunk_get(&p->wpos);

    /* No path, so do nothing. */
    if (path_n < 1) return 0;

    /*
     * The starting square is never drawn, but notice if it is being
     * displayed. In theory, it could be the last such square.
     */
    on_screen = panel_contains(p, grid);

    /* Draw the path. */
    for (i = 0; i < path_n; i++)
    {
        byte colour;
        struct object *obj = square_known_pile(p, c, &path_g[i]);
        struct source who_body;
        struct source *who = &who_body;

        /*
         * As path[] is a straight line and the screen is oblong,
         * there is only section of path[] on-screen.
         * If the square being drawn is visible, this is part of it.
         * If none of it has been drawn, continue until some of it
         * is found or the last square is reached.
         * If some of it has been drawn, finish now as there are no
         * more visible squares to draw.
         */
        if (panel_contains(p, &path_g[i])) on_screen = true;
        else if (on_screen) break;
        else continue;

        square_actor(c, &path_g[i], who);

        /* Once we pass an unknown square, we no longer know if we will reach later squares */
        if (pastknown)
            colour = COLOUR_L_DARK;

        /* Choose a colour (monsters). */
        else if (who->monster && monster_is_visible(p, who->idx))
        {
            /* Mimics act as objects */
            if (monster_is_mimicking(who->monster))
                colour = COLOUR_YELLOW;

            /* Visible monsters are red. */
            else
                colour = COLOUR_L_RED;
        }

        /* Choose a colour (players). */
        else if (who->player && player_is_visible(p, who->idx))
        {
            /* Player mimics act as objects (or features) */
            if (who->player->k_idx > 0)
                colour = COLOUR_YELLOW;
            else if (who->player->k_idx < 0)
                colour = COLOUR_WHITE;

            /* Visible players are red. */
            else
                colour = COLOUR_L_RED;
        }

        /* Known objects are yellow. */
        else if (obj)
            colour = COLOUR_YELLOW;

        /* Known walls are blue. */
        else if (!square_isprojectable(c, &path_g[i]) &&
            (square_isknown(p, &path_g[i]) || square_isseen(p, &path_g[i])))
        {
            colour = COLOUR_BLUE;
        }

        /* Unknown squares are grey. */
        else if (!square_isknown(p, &path_g[i]) && !square_isseen(p, &path_g[i]))
        {
            pastknown = true;
            colour = COLOUR_L_DARK;
        }

        /* Unoccupied squares are white. */
        else
            colour = COLOUR_WHITE;

        /* Draw the path segment */
        draw_path_grid(p, &path_g[i], colour, '*');
    }

    /* Flush and wait (delay for consistency) */
    if (i) Send_flush(p, true, true);
    else Send_flush(p, true, false);

    return i;
}


/*
 * Load the attr/char at each point along "path" which is on screen.
 */
void load_path(struct player *p, u16b path_n, struct loc *path_g)
{
    int i;

    for (i = 0; i < path_n; i++)
    {
        if (!panel_contains(p, &path_g[i])) continue;
        square_light_spot_aux(p, chunk_get(&p->wpos), &path_g[i]);
    }

    Send_flush(p, true, false);
    p->path_drawn = false;
}


/*
 * Extract a direction (or zero) from a character
 */
static int target_dir(u32b ch)
{
    int d = 0;

    /* Already a direction? */
    if (ch <= 9)
        d = ch;
    else if (isdigit((unsigned char)ch))
        d = D2I(ch);
    else if (isarrow(ch))
    {
        switch (ch)
        {
            case ARROW_DOWN:  d = 2; break;
            case ARROW_LEFT:  d = 4; break;
            case ARROW_RIGHT: d = 6; break;
            case ARROW_UP:    d = 8; break;
        }
    }

    /* Paranoia */
    if (d == 5) d = 0;

    /* Return direction */
    return (d);
}


static void set_tt_m(struct player *p, s16b m)
{
    p->tt_m = m;
    p->tt_o = NULL;
}


/*
 * Handle "target" and "look".
 *
 * Note that this code can be called from "get_aim_dir()".
 *
 * Currently, when "flag" is true, that is, when
 * "interesting" grids are being used, and a directional key is used, we
 * only scroll by a single panel, in the direction requested, and check
 * for any interesting grids on that panel.  The "correct" solution would
 * actually involve scanning a larger set of grids, including ones in
 * panels which are adjacent to the one currently scanned, but this is
 * overkill for this function.  XXX XXX
 *
 * Hack -- targeting/observing an "outer border grid" may induce
 * problems, so this is not currently allowed.
 *
 * The player can use the direction keys to move among "interesting"
 * grids in a heuristic manner, or the "space", "+", and "-" keys to
 * move through the "interesting" grids in a sequential manner, or
 * can enter "location" mode, and use the direction keys to move one
 * grid at a time in any direction.  The "t" (set target) command will
 * only target a monster (as opposed to a location) if the monster is
 * target_able and the "interesting" mode is being used.
 *
 * The current grid is described using the "look" method above, and
 * a new command may be entered at any time, but note that if the
 * "TARGET_LOOK" bit flag is set (or if we are in "location" mode,
 * where "space" has no obvious meaning) then "space" will scan
 * through the description of the current grid until done, instead
 * of immediately jumping to the next "interesting" grid.  This
 * allows the "target" command to retain its old semantics.
 *
 * The "*", "+", and "-" keys may always be used to jump immediately
 * to the next (or previous) interesting grid, in the proper mode.
 *
 * The "return" key may always be used to scan through a complete
 * grid description (forever).
 *
 * This command will cancel any old target, even if used from
 * inside the "look" command.
 *
 * 'mode' is one of TARGET_LOOK or TARGET_KILL.
 * 'x' and 'y' are the initial position of the target to be highlighted,
 * or -1 if no location is specified.
 * Returns true if a target has been successfully set, false otherwise.
 */
bool target_set_interactive(struct player *p, int mode, u32b query)
{
    int i, d, t, bd;
    bool done = false;
    struct point_set *targets;
    bool good_target;
    char help[NORMAL_WID];
    struct target old_target_body;
    struct target *old_target = &old_target_body;
    bool auto_target = false;
    int tries = 200;
    struct chunk *c = chunk_get(&p->wpos);

    /* Paranoia */
    if (!c) return false;

    /* Remove old targeting path */
    if (p->path_drawn) load_path(p, p->path_n, p->path_g);

    /* Hack -- auto-target if requested */
    if ((mode & (TARGET_AIM)) && OPT(p, use_old_target) && target_okay(p))
    {
        memcpy(old_target, &p->target, sizeof(struct target));
        auto_target = true;
    }

    if (query == '\0')
    {
        p->tt_flag = true;
        p->tt_step = TARGET_NONE;
        p->tt_help = false;
    }

    /* Start on the player */
    if (query == '\0')
    {
        loc_copy(&p->tt_grid, &p->grid);

        /* Hack -- auto-target if requested */
        if (auto_target) loc_copy(&p->tt_grid, &old_target->grid);
    }

    /* Cancel target */
	target_set_monster(p, NULL);

    /* Cancel tracking */
    cursor_track(p, NULL);

    /* Prepare the "temp" array */
    targets = target_get_monsters(p, mode);

    /* Start near the player */
    if (query == '\0')
    {
        set_tt_m(p, 0);

        /* Hack -- auto-target if requested */
        if (auto_target)
        {
            /* Find the old target */
            for (i = 0; i < point_set_size(targets); i++)
            {
                struct source temp_who_body;
                struct source *temp_who = &temp_who_body;

                square_actor(c, &targets->pts[i].grid, temp_who);

                if (source_equal(temp_who, &old_target->target_who))
                {
                    set_tt_m(p, i);
                    break;
                }
            }
        }
    }

    /* Interact */
    while (tries-- && !done)
    {
        bool ret;

        /* Paranoia: grids could have changed! */
        if (p->tt_m >= point_set_size(targets)) set_tt_m(p, point_set_size(targets) - 1);
        if (p->tt_m < 0) set_tt_m(p, 0);

#ifdef NOTARGET_PROMPT
        /* No targets */
        if (p->tt_flag && !point_set_size(targets))
        {
            /* Analyze */
            switch (query)
            {
                case ESCAPE:
                case 'q': break;

                case 'p':
                {
                    p->tt_flag = false;
                    break;
                }

                default:
                {
                    int col = p->grid.x - p->offset_grid.x;
                    int row = p->grid.y - p->offset_grid.y + 1;
                    bool dble = true;
                    struct loc grid;

                    loc_init(&grid, p->grid.x, p->grid.y - 1);

                    /* Hack -- is there something targetable above our position? */
                    if (square_in_bounds_fully(c, &grid) && target_accept(p, &grid))
                        dble = false;

                    Send_target_info(p, col, row, dble, "Nothing to target. [p,q]");
                    point_set_dispose(targets);
                    return false;
                }
            }
        }
#endif

        /* Interesting grids if chosen and there are any, otherwise arbitrary */
        if (p->tt_flag && point_set_size(targets))
        {
            struct source who_body;
            struct source *who = &who_body;

            loc_copy(&p->tt_grid, &targets->pts[p->tt_m].grid);

            /* Adjust panel if needed */
            if (adjust_panel_help(p, p->tt_grid.y, p->tt_grid.x)) handle_stuff(p);

            /* Update help */
            square_actor(c, &targets->pts[p->tt_m].grid, who);
            good_target = target_able(p, who);
            target_display_help(help, sizeof(help), good_target, false);

            /* Find the path. */
            p->path_n = project_path(p, p->path_g, z_info->max_range, c, &p->grid,
                &targets->pts[p->tt_m].grid, PROJECT_THRU | PROJECT_INFO);

            /* Draw the path in "target" mode. If there is one */
            if (mode & TARGET_KILL)
                p->path_drawn = draw_path(p, p->path_n, p->path_g, &p->grid);

            /* Describe and Prompt */
            ret = target_set_interactive_aux(p, &p->tt_grid, mode, help, query);

            if (ret)
            {
                point_set_dispose(targets);
                return false;
            }

            /* Remove the path */
            if (p->path_drawn) load_path(p, p->path_n, p->path_g);

            /* Assume no "direction" */
            d = 0;

            /* Analyze */
            switch (query)
            {
                case ESCAPE:
                case 'q':
                case 'r':
                {
                    done = true;
                    break;
                }

                case ' ':
                case '(':
                case '*':
                case '+':
                {
                    set_tt_m(p, p->tt_m + 1);
                    if (p->tt_m == point_set_size(targets)) set_tt_m(p, 0);

                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case '-':
                {
                    set_tt_m(p, p->tt_m - 1);
                    if (p->tt_m == -1) set_tt_m(p, point_set_size(targets) - 1);

                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case 'p':
                {
                    /* Recenter around player */
                    verify_panel(p);

                    /* Handle stuff */
                    handle_stuff(p);

                    loc_copy(&p->tt_grid, &p->grid);
                }

                /* fallthrough */
                case 'o':
                {
                    p->tt_flag = false;

                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case 'm':
                {
                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case 't':
                case '5':
                case '0':
                case '.':
                {
                    square_actor(c, &p->tt_grid, who);

                    if (target_able(p, who))
                    {
                        health_track(p->upkeep, who);
                        target_set_monster(p, who);
                    }

                    done = true;

                    break;
                }

                default:
                {
                    /* Extract direction */
                    d = target_dir(query);

                    /* Oops */
                    if (!d)
                    {
                        /* Hack -- acknowledge */
                        if (query != KC_ENTER) query = '\0';
                    }

                    break;
                }
            }

            /* Hack -- move around */
            if (d)
            {
                int old_y = targets->pts[p->tt_m].grid.y;
                int old_x = targets->pts[p->tt_m].grid.x;

                /* Find a new monster */
                i = target_pick(old_y, old_x, ddy[d], ddx[d], targets);

                /* Scroll to find interesting grid */
                if (i < 0)
                {
                    struct loc offset_grid;

                    loc_copy(&offset_grid, &p->offset_grid);

                    /* Change if legal */
                    if (change_panel(p, d))
                    {
                        /* Recalculate interesting grids */
                        point_set_dispose(targets);
                        targets = target_get_monsters(p, mode);

                        /* Find a new monster */
                        i = target_pick(old_y, old_x, ddy[d], ddx[d], targets);

                        /* Restore panel if needed */
                        if ((i < 0) && modify_panel(p, &offset_grid))
                        {
                            /* Recalculate interesting grids */
                            point_set_dispose(targets);
                            targets = target_get_monsters(p, mode);
                        }

                        /* Handle stuff */
                        handle_stuff(p);
                    }
                }

                /* Use interesting grid if found */
                if (i >= 0) set_tt_m(p, i);

                /* Hack -- acknowledge */
                query = '\0';
            }
        }
        else
        {
            struct source who_body;
            struct source *who = &who_body;

            /* Update help */
            square_actor(c, &p->tt_grid, who);
            good_target = target_able(p, who);
            target_display_help(help, sizeof(help), good_target, true);

            /* Find the path. */
            p->path_n = project_path(p, p->path_g, z_info->max_range, c, &p->grid, &p->tt_grid,
                PROJECT_THRU | PROJECT_INFO);

            /* Draw the path in "target" mode. If there is one */
            if (mode & TARGET_KILL)
                p->path_drawn = draw_path(p, p->path_n, p->path_g, &p->grid);

            /* Describe and Prompt (enable "TARGET_LOOK") */
            ret = target_set_interactive_aux(p, &p->tt_grid, mode | TARGET_LOOK,
                help, query);

            if (ret)
            {
                point_set_dispose(targets);
                return false;
            }

            /* Remove the path */
            if (p->path_drawn) load_path(p, p->path_n, p->path_g);

            /* Assume no "direction" */
            d = 0;

            /* Analyze */
            switch (query)
            {
                case ESCAPE:
                case 'q':
                case 'r':
                {
                    done = true;
                    break;
                }

                case ' ':
                case '(':
                case '*':
                case '+':
                case '-':
                {
                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case 'p':
                {
                    /* Recenter around player */
                    verify_panel(p);

                    /* Handle stuff */
                    handle_stuff(p);

                    loc_copy(&p->tt_grid, &p->grid);
                }

                case 'o':
                {
                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case 'm':
                {
                    p->tt_flag = true;

                    set_tt_m(p, 0);
                    bd = 999;

                    /* Pick a nearby monster */
                    for (i = 0; i < point_set_size(targets); i++)
                    {
                        t = distance(&p->tt_grid, &targets->pts[i].grid);

                        /* Pick closest */
                        if (t < bd)
                        {
                            set_tt_m(p, i);
                            bd = t;
                        }
                    }

                    /* Nothing interesting */
                    if (bd == 999) p->tt_flag = false;

                    /* Hack -- acknowledge */
                    query = '\0';

                    break;
                }

                case 't':
                case '5':
                case '0':
                case '.':
                {
                    target_set_location(p, &p->tt_grid);
                    done = true;

                    break;
                }

                default:
                {
                    /* Extract a direction */
                    d = target_dir(query);

                    /* Oops */
                    if (!d)
                    {
                        /* Hack -- acknowledge */
                        if (query != KC_ENTER) query = '\0';
                    }

                    break;
                }
            }

            /* Handle "direction" */
            if (d)
            {
                /* Move */
                p->tt_grid.x += ddx[d];
                p->tt_grid.y += ddy[d];

                /* Slide into legality */
                if (p->tt_grid.x >= c->width - 1) p->tt_grid.x--;
                else if (p->tt_grid.x <= 0) p->tt_grid.x++;

                /* Slide into legality */
                if (p->tt_grid.y >= c->height - 1) p->tt_grid.y--;
                else if (p->tt_grid.y <= 0) p->tt_grid.y++;

                /* Adjust panel if needed */
                if (adjust_panel_help(p, p->tt_grid.y, p->tt_grid.x))
                {
                    /* Handle stuff */
                    handle_stuff(p);

                    /* Recalculate interesting grids */
                    point_set_dispose(targets);
                    targets = target_get_monsters(p, mode);
                }

                /* Hack -- acknowledge */
                query = '\0';
            }
        }
    }

    /* Paranoia */
    if (!tries) plog_fmt("Infinite loop in target_set_interactive: %lu", query);

    /* Forget */
    point_set_dispose(targets);

    /* Recenter around player */
    verify_panel(p);

    /* Handle stuff */
    handle_stuff(p);

    /* Failure to set target */
    if (!p->target.target_set) return false;

    /* Success */
    return true;
}