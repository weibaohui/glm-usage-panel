/* ui.c — the dashboard: top bar, context pills, quota window cards, MCP and
 * account cards, model usage, settings sheet, onboarding, status bar.
 *
 * A Chinese interface needs CJK glyphs the embedded Latin fonts do not have,
 * so on the first frame we add a system CJK font as a fallback for every text
 * role (PingFang on macOS, Noto/WQY on Linux, 微软雅黑 on Windows). */
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "glm.h"

#define WINDOW_GAP 16
#define CONTENT_PAD 28

typedef struct {
    glm_client *glm;
    glm_config cfg;            /* the client's config, read once per frame */
    glm_snapshot snap;
    int fetch_gen;             /* last seen; a change fires toasts */
    int manual_refresh;        /* the current fetch was started by hand */
    double last_done_time;     /* cui_time() of the last finished fetch */
    double due_at;             /* auto-refresh schedule */

    bool settings_open;
    int toast_success, toast_error;

    /* settings editing state */
    glm_config edit;           /* saved off when the sheet opens */
    char edit_name[64], edit_org[80], edit_proj[80];
    int edit_type;             /* segmented: 0 personal, 1 team */
    int edit_index;            /* -1 = adding a new context */
    char edit_webjwt[2048];
    char edit_interval[16];    /* number input buffer */
    int discover_gen;

    bool fonts_ready;
    bool theme_touched;        /* the top-bar toggle wins over the config afterwards */
    bool demo;
} app;

/* ---------------------------------------------------------------- fonts */

static const char *cjk_candidates[] = {
    /* macOS */
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    /* Linux */
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    /* Windows */
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\simsun.ttc",
    "C:\\Windows\\Fonts\\simhei.ttf",
};

static void load_fonts(cui_ctx *ui, app *a) {
    a->fonts_ready = true;
    cui_font cjk = -1;
    for (size_t i = 0; i < sizeof cjk_candidates / sizeof *cjk_candidates; i++) {
        cjk = cui_font_load_file(ui, cjk_candidates[i]);
        if (cjk >= 0) break;   /* note: failure is -1, not 0 */
        cjk = -1;
    }
    if (cjk < 0) {
        fprintf(stderr, "glm: 没有找到可用的系统 CJK 字体，中文将无法显示\n");
        return;
    }
    cui_font_add_fallback(ui, CUI_FONT_BODY, cjk);
    cui_font_add_fallback(ui, CUI_FONT_MEDIUM, cjk);
    cui_font_add_fallback(ui, CUI_FONT_DISPLAY, cjk);
    cui_font_add_fallback(ui, CUI_FONT_MONO, cjk);
}

/* ------------------------------------------------------------- helpers */

static const char *usage_word(double pct) {
    int t = glm_usage_tone(pct);
    return t == 2 ? "接近上限" : t == 1 ? "用量偏高" : "余量充足";
}

static cui_tone tone_tone(int t) {
    return t == 2 ? CUI_TONE_DANGER : t == 1 ? CUI_TONE_WARNING : CUI_TONE_SUCCESS;
}

static void progress_tone(cui_ctx *ui, double pct) {
    cui_progress(ui, (float)pct, .tone = tone_tone(glm_usage_tone(pct)), .height = 8);
}

/* find the TOKENS_LIMIT-ish limits and label them: earliest reset = the 5h window */
static void split_windows(const glm_snapshot *s, const glm_limit **five, const glm_limit **week,
                          const glm_limit **mcp) {
    *five = *week = *mcp = NULL;
    for (int i = 0; i < s->limit_count; i++) {
        const glm_limit *l = &s->limits[i];
        if (!strcmp(l->kind, "TIME_LIMIT")) { *mcp = l; continue; }
        if (strcmp(l->kind, "TOKENS_LIMIT") && strcmp(l->kind, "CREDIT_LIMIT")) continue;
        if (!*five || l->next_reset < (*five)->next_reset) {
            *week = *five;
            *five = l;
        } else {
            *week = l;
        }
    }
}

/* window title: prefer the API's unit/number (3=hour, 6=week), else the caller's */
static void window_title(const glm_limit *l, const char *fallback, char *out, size_t cap) {
    if (l && l->unit == 3 && l->number > 0) {
        snprintf(out, cap, "%.0f 小时窗口", l->number);
        return;
    }
    if (l && l->unit == 6 && l->number > 0) {
        if (l->number == 1) snprintf(out, cap, "本周额度");
        else snprintf(out, cap, "%.0f 周窗口", l->number);
        return;
    }
    snprintf(out, cap, "%s", fallback);
}

/* what the remaining figure counts on this kind of limit */
static const char *remaining_unit(const glm_limit *l) {
    return l && !strcmp(l->kind, "TOKENS_LIMIT") ? "Tokens" : "积分";
}

static void window_card(cui_ctx *ui, const char *id, const char *fallback_title,
                        const glm_limit *l, long long now) {
    char title[64];
    window_title(l, fallback_title, title, sizeof title);
    cui_card(ui, .gap = 14, .width = CUI_GROW(1), .min_width = 320) {
        cui_row(ui) {
            cui_column(ui, .gap = 2, .width = CUI_FILL) {
                cui_label(ui, title);
                if (l && l->next_reset) {
                    char buf[96];
                    glm_fmt_reset(l->next_reset, now, buf, sizeof buf);
                    cui_text(ui, buf, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                } else {
                    cui_text(ui, "暂无重置时间", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                }
            }
            if (l) cui_badge(ui, usage_word(l->percentage), .tone = tone_tone(glm_usage_tone(l->percentage)),
                             .dot = true);
        }
        if (!l) {
            cui_column(ui, .height = CUI_PX(150), .align = CUI_ALIGN_CENTER,
                       .justify = CUI_JUSTIFY_CENTER) {
                cui_secondary(ui, "这个窗口没有返回数据");
            }
            return;
        }
        char remain[64];
        glm_human_tokens(l->remaining, remain, sizeof remain);
        static const cui_gauge_band bands[] = {
            { 60, CUI_TONE_SUCCESS }, { 85, CUI_TONE_WARNING }, { 100, CUI_TONE_DANGER } };
        cui_row(ui, .gap = 20, .align = CUI_ALIGN_CENTER) {
            cui_radial_gauge(ui, id, (float)l->percentage, 0, 100,
                             .size = 148, .unit = "%",
                             .label = "已用", .format = "%.1f", .ticks = -1,
                             .bands = bands, .band_count = 3, .no_needle = true);
            cui_column(ui, .gap = 8, .width = CUI_FILL) {
                cui_column(ui, .gap = 0) {
                    char cap_label[24];
                    snprintf(cap_label, sizeof cap_label, "剩余%s", remaining_unit(l));
                    cui_text(ui, cap_label, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_text(ui, remain, .font = CUI_FONT_DISPLAY, .size = 24, .tabular = true);
                    if (l->usage > 0) {
                        char used[96];
                        snprintf(used, sizeof used, "已用 %.0f · 共 %.0f", l->current_value,
                                 l->usage);
                        cui_text(ui, used, .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                    } else {
                        cui_text(ui, remaining_unit(l), .size = 11,
                                 .color = CUI_COLOR_TEXT_SECONDARY);
                    }
                }
                progress_tone(ui, l->percentage);
            }
        }
    }
}

static void meta_row(cui_ctx *ui, const char *icon, const char *label, const char *value) {
    cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, icon, .font = CUI_FONT_ICONS, .size = 14, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_text(ui, label, .size = 13, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
        cui_spacer(ui);
        cui_text(ui, value, .size = 13, .tabular = true, .nowrap = true);
    }
}

static void mcp_card(cui_ctx *ui, const glm_limit *mcp) {
    char used[32], total[32];
    glm_human_tokens(mcp->current_value, used, sizeof used);
    glm_human_tokens(mcp->usage, total, sizeof total);
    cui_card(ui, .gap = 12, .width = CUI_GROW(1), .min_width = 260) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, CUI_ICON_ACTIVITY, .font = CUI_FONT_ICONS, .size = 14,
                     .color = CUI_COLOR_ACCENT);
            cui_label(ui, "MCP 用量（月）");
            cui_spacer(ui);
            cui_badge(ui, "TIME_LIMIT", .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
        }
        cui_row(ui, .align = CUI_ALIGN_END, .gap = 8) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f", mcp->current_value);
            cui_text(ui, buf, .font = CUI_FONT_DISPLAY, .size = 26, .tabular = true);
            snprintf(buf, sizeof buf, "/ %.0f 次", mcp->usage);
            cui_text(ui, buf, .size = 13, .color = CUI_COLOR_TEXT_SECONDARY, .tabular = true);
        }
        progress_tone(ui, mcp->percentage);
        char pct[32];
        snprintf(pct, sizeof pct, "已用 %.1f%%", mcp->percentage);
        cui_text(ui, pct, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
    }
}

static void resets_card(cui_ctx *ui, const glm_snapshot *s) {
    char five[16], week[16];
    snprintf(five, sizeof five, "%d 张", s->five_resets);
    snprintf(week, sizeof week, "%d 张", s->week_resets);
    cui_card(ui, .gap = 12, .width = CUI_GROW(1), .min_width = 260) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, CUI_ICON_GIFT, .font = CUI_FONT_ICONS, .size = 14,
                     .color = CUI_COLOR_ACCENT);
            cui_label(ui, "重置券");
            cui_spacer(ui);
            cui_badge(ui, "额度重置", .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
        }
        meta_row(ui, CUI_ICON_CLOCK, "5 小时窗口", five);
        meta_row(ui, CUI_ICON_CALENDAR, "本周额度", week);
        cui_text(ui, "触顶时可用重置券立即清零对应窗口。", .size = 12,
                 .color = CUI_COLOR_TEXT_SECONDARY);
    }
}

static void account_card(cui_ctx *ui, const glm_snapshot *s) {
    char bal[32], give[32], spend[32];
    glm_human_money(s->balance, bal, sizeof bal);
    glm_human_money(s->give_amount, give, sizeof give);
    glm_human_money(s->total_spend, spend, sizeof spend);
    cui_card(ui, .gap = 14, .width = CUI_GROW(1)) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, CUI_ICON_WALLET, .font = CUI_FONT_ICONS, .size = 14,
                     .color = CUI_COLOR_ACCENT);
            cui_label(ui, "账户用量");
            cui_spacer(ui);
            cui_badge(ui, s->no_plan ? "无编程套餐" : "账户口径",
                      .tone = s->no_plan ? CUI_TONE_WARNING : CUI_TONE_NEUTRAL,
                      .size = CUI_SIZE_SM);
        }
        cui_row(ui, .gap = 24, .wrap = true) {
            cui_column(ui, .gap = 2) {
                cui_text(ui, "账户余额", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, bal, .font = CUI_FONT_DISPLAY, .size = 24, .tabular = true);
            }
            cui_column(ui, .gap = 2) {
                cui_text(ui, "赠送额度", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, give, .font = CUI_FONT_DISPLAY, .size = 24, .tabular = true);
            }
            cui_column(ui, .gap = 2) {
                cui_text(ui, "累计消耗", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, spend, .font = CUI_FONT_DISPLAY, .size = 24, .tabular = true);
            }
        }
        if (s->no_plan)
            cui_alert(ui, "当前机构没有购买编程套餐，以上为按量付费的账户口径。",
                      .tone = CUI_TONE_WARNING, .icon = CUI_ICON_INFO);
    }
}

static void models_card(cui_ctx *ui, const glm_snapshot *s) {
    cui_card(ui, .gap = 14, .width = CUI_FILL) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .gap = 2, .width = CUI_FILL) {
                cui_label(ui, "模型用量 · 最近 24 小时");
                cui_text(ui, "按 Tokens 消耗排序", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_badge(ui, "best effort", .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
        }
        static float vals[7];
        static const char *names[7];
        static char name_store[7][48];
        int n = 0;
        double other = 0;
        for (int i = 0; i < s->model_count; i++) {
            if (n < 6 && i < 6 && s->models[i].tokens > 0) {
                vals[n] = (float)(s->models[i].tokens / 1e4); /* donut unit: 万 tokens */
                snprintf(name_store[n], sizeof name_store[n], "%s", s->models[i].model);
                names[n] = name_store[n];
                n++;
            } else {
                other += s->models[i].tokens;
            }
        }
        if (other > 0 && n < 7) {
            vals[n] = (float)(other / 1e4);
            snprintf(name_store[n], sizeof name_store[n], "其他");
            names[n] = name_store[n];
            n++;
        }
        cui_row(ui, .gap = 24, .align = CUI_ALIGN_CENTER, .wrap = true) {
            if (n > 1) cui_donut_chart(ui, "models", vals, names, n, .height = 190,
                                       .format = "%.0f 万");
            cui_column(ui, .gap = 0, .width = CUI_GROW(1), .min_width = 260) {
                static const cui_table_column cols[] = {
                    { "模型", CUI_FILL, CUI_TEXT_LEFT, false },
                    { "Tokens", CUI_PX(120), CUI_TEXT_RIGHT, false },
                    { "请求", CUI_PX(90), CUI_TEXT_RIGHT, false },
                };
                static cui_table_sort sort;
                cui_table(ui, "modeltable", cols, 3, .sort = &sort, .compact = true) {
                    for (int i = 0; i < s->model_count && i < 8; i++) {
                        char tok[32], req[32];
                        glm_human_tokens(s->models[i].tokens, tok, sizeof tok);
                        snprintf(req, sizeof req, "%.0f", s->models[i].requests);
                        cui_table_row(ui, false);
                        cui_table_cell_text(ui, s->models[i].model);
                        cui_table_cell_text(ui, tok);
                        cui_table_cell_text(ui, req);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------- settings */

static void settings_sync_edit(app *a) {
    if (a->edit_index >= 0 && a->edit_index < a->edit.context_count) {
        const glm_context *c = &a->edit.contexts[a->edit_index];
        snprintf(a->edit_name, sizeof a->edit_name, "%s", c->name);
        snprintf(a->edit_org, sizeof a->edit_org, "%s", c->org);
        snprintf(a->edit_proj, sizeof a->edit_proj, "%s", c->project);
        a->edit_type = c->project_type == 1 ? 0 : 1;
    }
}

static void settings_begin(app *a) {
    glm_client_get_config(a->glm, &a->edit);
    a->edit_index = a->edit.selected < a->edit.context_count ? a->edit.selected : -1;
    settings_sync_edit(a);
    a->edit_webjwt[0] = '\0';
    snprintf(a->edit_interval, sizeof a->edit_interval, "%d", a->edit.refresh_seconds);
    a->settings_open = true;
}

static void settings_apply(app *a) {
    int seconds = atoi(a->edit_interval);
    if (seconds < 15) seconds = 15;
    a->edit.refresh_seconds = seconds;
    a->edit.selected = a->edit_index >= 0 ? a->edit_index : 0;
    glm_config_save(&a->edit);
    glm_client_apply_config(a->glm, &a->edit);
    a->manual_refresh = 0;
    a->due_at = 0;
    a->settings_open = false;
}

static void context_form(cui_ctx *ui, app *a) {
    cui_input(ui, "ctxname", a->edit_name, sizeof a->edit_name, .label = "名称",
              .placeholder = "团队编程套餐项目");
    cui_row(ui, .gap = 10) {
        cui_input(ui, "ctxorg", a->edit_org, sizeof a->edit_org, .label = "机构 ID",
                  .placeholder = "org-…", .width = CUI_FILL);
        cui_input(ui, "ctxproj", a->edit_proj, sizeof a->edit_proj, .label = "项目 ID",
                  .placeholder = "proj-…", .width = CUI_FILL);
    }
    static const char *types[] = { "个人机构", "团队编程套餐" };
    cui_segmented(ui, "ctxtype", types, 2, &a->edit_type, .size = CUI_SIZE_SM);
    cui_row(ui, .gap = 8) {
        if (cui_button(ui, "保存到列表", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) {
            glm_context *dst = NULL;
            if (a->edit_index >= 0 && a->edit_index < a->edit.context_count) {
                dst = &a->edit.contexts[a->edit_index];
            } else if (a->edit.context_count < GLM_MAX_CONTEXTS) {
                a->edit_index = a->edit.context_count++;
                dst = &a->edit.contexts[a->edit_index];
            }
            if (dst) {
                snprintf(dst->name, sizeof dst->name, "%.63s",
                         a->edit_name[0] ? a->edit_name : "未命名上下文");
                snprintf(dst->org, sizeof dst->org, "%.79s", a->edit_org);
                snprintf(dst->project, sizeof dst->project, "%.79s", a->edit_proj);
                dst->project_type = a->edit_type == 0 ? 1 : 2;
            }
        }
        if (a->edit_index >= 0 &&
            cui_button(ui, "清空后新建", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) {
            a->edit_index = -1;
            a->edit_name[0] = a->edit_org[0] = a->edit_proj[0] = '\0';
        }
    }
}

static void settings_sheet(cui_ctx *ui, app *a) {
    if (!a->settings_open) return;
    cui_sheet(ui, "设置", &a->settings_open, .side = CUI_SIDE_RIGHT, .size = 500) {
        cui_column(ui, .height = CUI_FILL, .gap = 0) {
            cui_scroll(ui, .id = "settings_scroll", .pad = 20, .gap = 16) {
                /* --- auth --- */
                cui_column(ui, .gap = 10) {
                    cui_label(ui, "认证方式");
                    static const char *modes[] = { "API Key（现算 JWT）", "现成 Token" };
                    cui_segmented(ui, "authmode", modes, 2, &a->edit.auth_mode, .size = CUI_SIZE_SM);
                    if (a->edit.auth_mode == GLM_AUTH_APIKEY) {
                        cui_input(ui, "apikey", a->edit.api_key, sizeof a->edit.api_key,
                                  .label = "API Key", .placeholder = "xxxxxxxx.yyyyyyyy",
                                  .password = true, .icon = CUI_ICON_KEY_ROUND,
                                  .hint = "格式 <id>.<secret>；每次请求都用它在本地现算一个短时效 JWT");
                    } else {
                        cui_input(ui, "token", a->edit.token, sizeof a->edit.token,
                                  .label = "Token（JWT / Coding Plan Token）",
                                  .placeholder = "eyJhbGci…", .password = true,
                                  .icon = CUI_ICON_KEY_ROUND,
                                  .hint = "直接作为 Authorization 发送，适合现成的 Coding Plan Token");
                    }
                }
                cui_separator(ui);
                /* --- endpoints --- */
                cui_column(ui, .gap = 10) {
                    cui_label(ui, "服务地址");
                    cui_input(ui, "baseurl", a->edit.base_url, sizeof a->edit.base_url,
                              .label = "API 地址",
                              .hint = "大陆站 open.bigmodel.cn · 国际站 api.z.ai");
                    cui_input(ui, "weburl", a->edit.web_url, sizeof a->edit.web_url,
                              .label = "发现用的站点",
                              .hint = "getCustomerInfo 所在站点，一般保持 bigmodel.cn");
                }
                cui_separator(ui);
                /* --- contexts --- */
                cui_column(ui, .gap = 10) {
                    cui_row(ui) {
                        cui_label(ui, "机构 / 项目上下文");
                        cui_spacer(ui);
                        char buf[16];
                        snprintf(buf, sizeof buf, "%d 条", a->edit.context_count);
                        cui_badge(ui, buf, .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
                    }
                    for (int i = 0; i < a->edit.context_count; i++) {
                        char id[16];
                        snprintf(id, sizeof id, "%d", i);
                        cui_push_id(ui, id);
                        glm_context *c = &a->edit.contexts[i];
                        bool is_sel = a->edit_index == i;
                        cui_card(ui, .dir = CUI_ROW, .gap = 10, .pad = 12, .align = CUI_ALIGN_CENTER,
                                 .selected = is_sel) {
                            cui_column(ui, .gap = 2, .width = CUI_FILL) {
                                cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                                    cui_text(ui, c->name, .font = CUI_FONT_MEDIUM, .size = 13,
                                             .nowrap = true);
                                    cui_badge(ui, c->project_type == 2 ? "团队" : "个人",
                                              .tone = c->project_type == 2 ? CUI_TONE_ACCENT
                                                                           : CUI_TONE_NEUTRAL,
                                              .size = CUI_SIZE_SM);
                                }
                                char ids[200];
                                snprintf(ids, sizeof ids, "%s%s%s", c->org,
                                         c->org[0] && c->project[0] ? " / " : "", c->project);
                                cui_text(ui, ids, .size = 11, .color = CUI_COLOR_TEXT_SECONDARY,
                                         .nowrap = true);
                            }
                            if (cui_button(ui, "编辑", .size = CUI_SIZE_SM,
                                           .variant = CUI_BUTTON_GHOST)) {
                                a->edit_index = i;
                                settings_sync_edit(a);
                            }
                            if (cui_icon_button(ui, CUI_ICON_TRASH_2, .size = CUI_SIZE_SM,
                                                .tooltip = "删除")) {
                                memmove(&a->edit.contexts[i], &a->edit.contexts[i + 1],
                                        sizeof(glm_context) *
                                            (size_t)(a->edit.context_count - i - 1));
                                a->edit.context_count--;
                                if (a->edit_index == i) a->edit_index = -1;
                                else if (a->edit_index > i) a->edit_index--;
                            }
                        }
                        cui_pop_id(ui);
                    }

                    cui_separator(ui);
                    cui_label(ui, a->edit_index >= 0 ? "编辑选中的上下文" : "添加上下文");
                    context_form(ui, a);
                    /* discovery */
                    cui_separator(ui);
                    cui_input(ui, "webjwt", a->edit_webjwt, sizeof a->edit_webjwt,
                              .label = "网页 JWT（可选，一次性）",
                              .placeholder = "粘贴后点击发现，自动拉取机构列表",
                              .password = true,
                              .hint = "bigmodel.cn 登录后的接口 Token，仅用于发现机构，成功后即丢弃");
                    cui_row(ui) {
                        cui_spacer(ui);
                        if (cui_button(ui, glm_client_discovering(a->glm) ? "发现中…" : "发现机构",
                                       .icon = CUI_ICON_SPARKLES, .size = CUI_SIZE_SM,
                                       .disabled = glm_client_discovering(a->glm)))
                            glm_client_discover(a->glm, a->edit_webjwt);
                    }
                }
                cui_separator(ui);
                /* --- refresh --- */
                cui_column(ui, .gap = 10) {
                    cui_label(ui, "刷新与外观");
                    cui_input(ui, "interval", a->edit_interval, sizeof a->edit_interval,
                              .label = "自动刷新间隔", .suffix = "秒", .hint = "最小 15 秒");
                    bool auto_refresh = a->edit.auto_refresh != 0;
                    if (cui_switch(ui, "自动刷新", &auto_refresh))
                        a->edit.auto_refresh = auto_refresh ? 1 : 0;
                    bool dark = a->edit.dark != 0;
                    if (cui_switch(ui, "深色主题（保存后生效）", &dark)) a->edit.dark = dark ? 1 : 0;
                }
            }
            cui_separator(ui);
            cui_row(ui, .pad_x = 20, .pad_y = 14, .gap = 8, .justify = CUI_JUSTIFY_END) {
                if (cui_button(ui, "取消")) a->settings_open = false;
                if (cui_button(ui, "保存并刷新", .variant = CUI_BUTTON_PRIMARY)) settings_apply(a);
            }
        }
    }
}

/* ------------------------------------------------------------ main view */

static void context_pills(cui_ctx *ui, app *a) {
    if (a->cfg.context_count <= 1) return;
    cui_row(ui, .gap = 8, .wrap = true) {
        for (int i = 0; i < a->cfg.context_count; i++) {
            char id[16];
            snprintf(id, sizeof id, "ctx%d", i);
            cui_push_id(ui, id);
            char label[96];
            snprintf(label, sizeof label, "%s · %s", a->cfg.contexts[i].name,
                     a->cfg.contexts[i].project_type == 2 ? "团队" : "个人");
            bool sel = a->cfg.selected == i;
            bool changed = cui_chip(ui, label, &sel);
            if (changed && sel && a->cfg.selected != i) {
                a->cfg.selected = i;
                glm_config_save(&a->cfg);
                glm_client_apply_config(a->glm, &a->cfg);
                a->manual_refresh = 0;
                a->due_at = 0;
            }
            cui_pop_id(ui);
        }
    }
}

static void dashboard(cui_ctx *ui, app *a) {
    long long now = (long long)time(NULL);
    const glm_snapshot *s = &a->snap;
    const glm_limit *five, *week, *mcp;
    split_windows(s, &five, &week, &mcp);

    if (s->no_plan) {
        cui_alert(ui, "当前机构没有编程套餐，已降级为账户视图。", .tone = CUI_TONE_WARNING,
                  .icon = CUI_ICON_INFO);
    }
    if (s->hint[0]) cui_alert(ui, s->hint, .tone = CUI_TONE_INFO, .icon = CUI_ICON_INFO);

    if (s->limit_count == 0 && !s->no_plan) {
        if (cui_empty_state(ui, "还没有额度数据", .icon = CUI_ICON_CHART_BAR,
                            .description = "接口没有返回额度窗口。检查认证与机构上下文，或稍后再试。",
                            .action = "重新查询"))
            glm_client_refresh(a->glm);
        return;
    }

    if (five || week) {
        cui_row(ui, .gap = WINDOW_GAP, .align = CUI_ALIGN_STRETCH, .wrap = true, .width = CUI_FILL) {
            window_card(ui, "win5", "5 小时窗口", five, now);
            window_card(ui, "winweek", "本周额度", week, now);
        }
    }
    if (mcp || s->has_resets) {
        cui_row(ui, .gap = WINDOW_GAP, .align = CUI_ALIGN_STRETCH, .wrap = true, .width = CUI_FILL) {
            if (mcp) mcp_card(ui, mcp);
            if (s->has_resets) resets_card(ui, s);
        }
    }
    if (s->has_account) account_card(ui, s);
    if (s->model_count > 0) models_card(ui, s);
}

static void onboarding(cui_ctx *ui, app *a) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER,
               .justify = CUI_JUSTIFY_CENTER, .gap = 14) {
        cui_box(ui, .width = CUI_PX(64), .height = CUI_PX(64), .radius = 18,
                .bg = CUI_COLOR_SURFACE_MUTED, .dir = CUI_STACK, .align = CUI_ALIGN_CENTER,
                .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, CUI_ICON_SPARKLES, .font = CUI_FONT_ICONS, .size = 30,
                     .color = CUI_COLOR_ACCENT);
        }
        cui_heading(ui, "先配置认证信息", 3);
        cui_text(ui, "填入 API Key（<id>.<secret>）或现成 Token，即可查看",
                 .color = CUI_COLOR_TEXT_SECONDARY);
        cui_text(ui, "5 小时窗口 / 本周额度、MCP 用量、重置券与账户消耗。",
                 .color = CUI_COLOR_TEXT_SECONDARY);
        if (cui_button(ui, "打开设置", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_SETTINGS))
            settings_begin(a);
    }
}

static void trigger_refresh(app *a, bool manual) {
    glm_client_refresh(a->glm);
    if (manual) { a->manual_refresh = 1; a->due_at = 0; }
}

static void topbar(cui_ctx *ui, app *a) {
    bool loading = glm_client_state(a->glm) == GLM_LOADING;
    double now_t = cui_time(ui);
    int countdown = 0;
    if (a->cfg.auto_refresh && a->due_at > 0 && !loading)
        countdown = (int)(a->due_at - now_t) > 0 ? (int)(a->due_at - now_t) : 0;

    cui_row(ui, .gap = 12, .pad_x = CONTENT_PAD, .pad_y = 14, .width = CUI_FILL,
            .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_PX(30), .height = CUI_PX(30), .radius = 9,
                .bg = CUI_COLOR_ACCENT, .dir = CUI_STACK, .align = CUI_ALIGN_CENTER,
                .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, "G", .font = CUI_FONT_DISPLAY, .size = 15,
                     .color = CUI_COLOR_ACCENT_FOREGROUND);
        }
        cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER, .width = CUI_GROW(1)) {
            cui_heading(ui, "GLM 用量面板", 3);
            if (a->snap.level[0])
                cui_badge(ui, a->snap.level, .tone = CUI_TONE_ACCENT, .solid = true,
                          .size = CUI_SIZE_SM);
            if (a->demo)
                cui_badge(ui, "演示数据", .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
        }
        if (countdown > 0) {
            char buf[32];
            snprintf(buf, sizeof buf, "%d 秒后自动刷新", countdown);
            cui_text(ui, buf, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        if (loading) cui_spinner(ui, 18);
        if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .tooltip = "立即刷新（⌘R）",
                            .disabled = loading))
            trigger_refresh(a, true);
        if (cui_icon_button(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_ICON_SUN
                                                                        : CUI_ICON_MOON,
                            .tooltip = "切换深浅主题")) {
            cui_set_theme(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_THEME_LIGHT
                                                                      : CUI_THEME_DARK);
            a->theme_touched = true;
        }
        if (cui_icon_button(ui, CUI_ICON_SETTINGS, .tooltip = "设置")) settings_begin(a);
    }
}

static void statusbar(cui_ctx *ui, app *a) {
    cui_row(ui, .gap = 10, .pad_x = CONTENT_PAD, .pad_y = 10, .width = CUI_FILL,
            .align = CUI_ALIGN_CENTER, .bg = CUI_COLOR_SURFACE_MUTED) {
        if (glm_client_state(a->glm) == GLM_LOADING) {
            cui_spinner(ui, 12);
            cui_text(ui, "正在查询…", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        } else if (a->snap.fetched_at) {
            char clock[16], buf[48];
            glm_fmt_clock(a->snap.fetched_at, clock, sizeof clock);
            snprintf(buf, sizeof buf, "上次更新 %s", clock);
            cui_text(ui, buf, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        } else {
            cui_text(ui, "尚未查询", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_spacer(ui);
        const glm_context *cur =
            a->cfg.context_count && a->cfg.selected < a->cfg.context_count
                ? &a->cfg.contexts[a->cfg.selected]
                : NULL;
        if (cur) {
            char ctxinfo[220];
            snprintf(ctxinfo, sizeof ctxinfo, "%s%s%s", cur->name, cur->org[0] ? " · " : "",
                     cur->org[0] ? cur->org : "");
            cui_text(ui, ctxinfo, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
        }
    }
}

static void frame(cui_ctx *ui, void *user) {
    app *a = user;
    if (!a->fonts_ready) load_fonts(ui, a);

    /* read the client state once per frame */
    glm_client_get_config(a->glm, &a->cfg);
    glm_client_snapshot(a->glm, &a->snap);
    /* the config's theme applies once at startup, unless CUI_THEME forced one */
    static int theme_started = 0;
    if (!theme_started) {
        theme_started = 1;
        if (!getenv("CUI_THEME"))
            cui_set_theme(ui, a->cfg.dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
    }

    /* a finished fetch: toasts, reschedule auto refresh */
    int gen = glm_client_fetch_gen(a->glm);
    if (gen != a->fetch_gen) {
        bool was_manual = a->manual_refresh;
        a->manual_refresh = 0;
        a->fetch_gen = gen;
        a->last_done_time = cui_time(ui);
        a->due_at = a->last_done_time + a->cfg.refresh_seconds;
        if (glm_client_state(a->glm) == GLM_ERROR) {
            a->toast_error = cui_toast(ui, "查询失败", .description = glm_client_error(a->glm),
                                       .tone = CUI_TONE_DANGER, .duration = was_manual ? 8 : 5);
        } else if (was_manual) {
            a->toast_success = cui_toast(ui, "已刷新", .tone = CUI_TONE_SUCCESS, .duration = 3);
        }
    }

    /* auto refresh */
    double now_t = cui_time(ui);
    bool configured = a->demo || (a->cfg.auth_mode == GLM_AUTH_APIKEY ? a->cfg.api_key[0] != '\0'
                                                                      : a->cfg.token[0] != '\0');
    if (a->cfg.auto_refresh && configured && !a->demo && a->due_at > 0 && now_t >= a->due_at &&
        glm_client_state(a->glm) != GLM_LOADING) {
        glm_client_refresh(a->glm);
        a->due_at = 0;
    }
    cui_request_wakeup(ui, 1.0); /* keep the countdown ticking */

    if (!cui_wants_keyboard(ui)) {
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_R) &&
            glm_client_state(a->glm) != GLM_LOADING)
            trigger_refresh(a, true);
    }

    /* layout */
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_STRETCH,
            .bg = CUI_COLOR_BACKGROUND) {
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
            topbar(ui, a);
            cui_separator(ui);
            cui_scroll(ui, .id = "content", .pad = CONTENT_PAD, .gap = WINDOW_GAP,
                       .fade_edges = true) {
                context_pills(ui, a);
                int state = glm_client_state(a->glm);
                if (!configured) {
                    onboarding(ui, a);
                } else if (state == GLM_LOADING && a->fetch_gen == 0) {
                    cui_column(ui, .gap = WINDOW_GAP) {
                        cui_row(ui, .gap = WINDOW_GAP) {
                            cui_column(ui, .gap = 12, .width = CUI_GROW(1)) {
                                cui_skeleton(ui, CUI_PX(140), 18);
                                cui_skeleton(ui, CUI_FILL, 200);
                            }
                            cui_column(ui, .gap = 12, .width = CUI_GROW(1)) {
                                cui_skeleton(ui, CUI_PX(140), 18);
                                cui_skeleton(ui, CUI_FILL, 200);
                            }
                        }
                        cui_skeleton(ui, CUI_FILL, 130);
                        cui_skeleton(ui, CUI_FILL, 190);
                    }
                } else if (state == GLM_ERROR && a->fetch_gen == 0) {
                    cui_alert(ui, glm_client_error(a->glm), .tone = CUI_TONE_DANGER,
                              .icon = CUI_ICON_CIRCLE_ALERT);
                    if (cui_button(ui, "打开设置", .icon = CUI_ICON_SETTINGS)) settings_begin(a);
                } else {
                    dashboard(ui, a);
                }
            }
            cui_separator(ui);
            statusbar(ui, a);
        }
    }

    settings_sheet(ui, a);

    /* discovery results land in the settings form */
    if (a->settings_open && !glm_client_discovering(a->glm) &&
        glm_client_discover_gen(a->glm) != a->discover_gen) {
        a->discover_gen = glm_client_discover_gen(a->glm);
        char err[256];
        glm_context found[GLM_MAX_CONTEXTS];
        int n = glm_client_discover_result(a->glm, found, GLM_MAX_CONTEXTS, err, sizeof err);
        if (n > 0) {
            memcpy(a->edit.contexts, found, sizeof(glm_context) * (size_t)n);
            a->edit.context_count = n;
            int pick = -1;
            for (int i = 0; i < n; i++)
                if (a->edit.contexts[i].project_type == 2) { pick = i; break; }
            a->edit_index = pick >= 0 ? pick : 0;
            settings_sync_edit(a);
            a->edit_webjwt[0] = '\0'; /* one-time use: drop it */
            cui_toast(ui, "发现完成", .description = "已更新机构列表", .tone = CUI_TONE_SUCCESS,
                      .duration = 4);
        } else if (err[0]) {
            cui_toast(ui, "发现失败", .description = err, .tone = CUI_TONE_DANGER, .duration = 6);
        }
    }
}

void ui_run(glm_client *glm, bool demo) {
    static app a;
    a.glm = glm;
    a.demo = demo;
    glm_client_get_config(glm, &a.cfg);
    if (getenv("GLM_UI_OPEN_SETTINGS")) settings_begin(&a); /* debug aid */
    cui_app_run(&(cui_app_desc){
        .title = "GLM 用量面板",
        .width = 1180,
        .height = 800,
        .min_width = 620,
        .min_height = 420,
        .accent = CUI_ACCENT_BLUE,
        .follow_system_theme = false,
        .theme = CUI_THEME_DARK,
    }, frame, &a);
}
