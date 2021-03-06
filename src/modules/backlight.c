#include <module/module_easy.h>
#include <polkit.h>
#include <udev.h>

#ifdef DDC_PRESENT

#include <ddcutil_c_api.h>

#define DDCUTIL_LOOP(func) \
    const DDCA_Vcp_Feature_Code br_code = 0x10; \
    DDCA_Display_Info_List *dlist = NULL; \
    ddca_get_display_info_list2(false, &dlist); \
    if (dlist) { \
        for (int ndx = 0; ndx < dlist->ct; ndx++) { \
            DDCA_Display_Info *dinfo = &dlist->info[ndx]; \
            DDCA_Display_Ref dref = dinfo->dref; \
            DDCA_Display_Handle dh = NULL; \
            if (ddca_open_display2(dref, false, &dh)) { \
                continue; \
            } \
            DDCA_Any_Vcp_Value *valrec; \
            if (!ddca_get_any_vcp_value_using_explicit_type(dh, br_code, DDCA_NON_TABLE_VCP_VALUE, &valrec)) { \
                func; \
                ddca_free_any_vcp_value(valrec); \
            } \
            ddca_close_display(dh); \
        } \
        ddca_free_display_info_list(dlist); \
    }

#define DDCUTIL_FUNC(sn, func) \
    const DDCA_Vcp_Feature_Code br_code = 0x10; \
    DDCA_Display_Identifier pdid = NULL; \
    DDCA_Display_Ref dref = NULL; \
    DDCA_Display_Handle dh = NULL; \
    DDCA_Any_Vcp_Value *valrec = NULL; \
    if (ddca_create_mfg_model_sn_display_identifier(NULL, NULL, sn, &pdid)) { \
        goto end; \
    } \
    if (ddca_create_display_ref(pdid, &dref)) { \
        goto end; \
    } \
    if (ddca_open_display2(dref, false, &dh)) { \
        goto end; \
    } \
    if (!ddca_get_any_vcp_value_using_explicit_type(dh, br_code, DDCA_NON_TABLE_VCP_VALUE, &valrec)) { \
        func; \
        ddca_free_any_vcp_value(valrec); \
    } \
end: \
    if (dh) { \
        ddca_close_display(dh); \
    } \
    if (dref) { \
        ddca_free_display_ref(dref); \
    } \
    if (pdid) { \
        ddca_free_display_identifier(pdid); \
    }

#else

#define DDCUTIL_LOOP(func) do {} while(0)
#define DDCUTIL_FUNC(sn, func) do {} while(0)

#endif

typedef struct {
    char *sn;
    int reached_target;
} device;

typedef struct {
    double target_pct;
    double smooth_step;
    unsigned int smooth_wait;
    device *d;
    int num_dev;
    double verse;
} smooth_change;

static int method_setallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_getallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_raiseallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_lowerallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void reset_backlight_struct(double target_pct, int is_smooth, double smooth_step, unsigned int smooth_wait, int verse);
static void add_backlight_sn(const char *sn, int internal);
static int set_internal_backlight(int idx);
static int set_external_backlight(int idx);
static void append_backlight(sd_bus_message *reply, const char *name, const double pct);
static int append_internal_backlight(sd_bus_message *reply, const char *path);
static int append_external_backlight(sd_bus_message *reply, const char *sn);

static smooth_change sc;
static int smooth_fd;
static const char object_path[] = "/org/clightd/clightd/Backlight";
static const char bus_interface[] = "org.clightd.clightd.Backlight";
static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SetAll", "d(bdu)s", "b", method_setallbrightness, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetAll", "s", "a(sd)", method_getallbrightness, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RaiseAll", "d(bdu)s", "b", method_raiseallbrightness, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("LowerAll", "d(bdu)s", "b", method_lowerallbrightness, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

MODULE("BACKLIGHT");

static void module_pre_start(void) {

}

static bool check(void) {
    return true;
}

static bool evaluate(void) {
    return true;
}

static void init(void) {
    int r = sd_bus_add_object_vtable(bus,
                                 NULL,
                                 object_path,
                                 bus_interface,
                                 vtable,
                                 NULL);
    if (r < 0) {
        m_log("Failed to issue method call: %s\n", strerror(-r));
    } else {
        smooth_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        m_register_fd(smooth_fd, true, NULL);
    }
}

static void receive(const msg_t *msg, const void *userdata) {
    uint64_t t;
    // nonblocking mode!
    if (!msg || !msg->is_pubsub) {
        read(smooth_fd, &t, sizeof(uint64_t));

        int reached_count = 0;
        for (int i = 0; i < sc.num_dev; reached_count += sc.d[i].reached_target, i++) {
            if (!sc.d[i].reached_target) {
                int ret = set_internal_backlight(i);
                // error: it was not an internal backlight interface
                if (ret == -1) {
                    // try to use it as external backlight sn
                    set_external_backlight(i);
                }
            }
        }

        struct itimerspec timerValue = {{0}};
        if (reached_count != sc.num_dev) {
            timerValue.it_value.tv_sec = 0;
            timerValue.it_value.tv_nsec = 1000 * 1000 * sc.smooth_wait; // ms
        } else {
            m_log("Reached target backlight: %s%.2lf.\n", sc.verse > 0 ? "+" : (sc.verse < 0 ? "-" : ""), sc.target_pct);
            /* Free all resources */
            reset_backlight_struct(0, 0, 0, 0, 0);
        }
        int ret = timerfd_settime(smooth_fd, 0, &timerValue, NULL);
        if (userdata) {
            *(int *)userdata = ret;
        }
    }
}

static void destroy(void) {

}

static void reset_backlight_struct(double target_pct, int is_smooth, double smooth_step, unsigned int smooth_wait, int verse) {
    if (sc.d) {
        for (int i = 0; i < sc.num_dev; i++) {
            free(sc.d[i].sn);
        }
        free(sc.d);
        sc.d = NULL;
    }
    sc.smooth_step = is_smooth ? smooth_step : 0.0;
    sc.smooth_wait = is_smooth ? smooth_wait : 0;
    sc.target_pct = target_pct;
    sc.num_dev = 0;
    sc.verse = verse;
}

static void add_backlight_sn(const char *sn, int internal) {
    int ok = !internal;

    /* Properly check internal interface exists before adding it */
    if (internal) {
        struct udev_device *dev = NULL;
        get_udev_device(sn, "backlight", NULL, NULL, &dev);
        if (dev) {
            ok = 1;
            udev_device_unref(dev);
        }
    }

    if (ok) {
        sc.d = realloc(sc.d, sizeof(device) * ++sc.num_dev);
        sc.d[sc.num_dev - 1].reached_target = 0;
        if (sn) {
            sc.d[sc.num_dev - 1].sn = strdup(sn);
        } else {
            sc.d[sc.num_dev - 1].sn = NULL;
        }
    }
}

static int method_setallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    if (!check_authorization(m)) {
        sd_bus_error_set_errno(ret_error, EPERM);
        return -EPERM;
    }

    const char *backlight_interface = NULL;
    double target_pct, smooth_step;
    const int is_smooth;
    const unsigned int smooth_wait;

    int r = sd_bus_message_read(m, "d(bdu)s", &target_pct, &is_smooth, &smooth_step,
                                &smooth_wait, &backlight_interface);
    if (r >= 0) {
        /** Sanity checks **/
        if (target_pct > 1.0) {
            target_pct = 1.0;
        } else if (target_pct < 0.0) {
            target_pct = 0.0;
        }
        
        if (smooth_step > 1.0) {
            smooth_step = 1.0;
        } else if (smooth_step <= 0.0) {
            smooth_step = 0.0; // disable smoothing
        }
        /** End of sanity checks **/
        
        int verse = 0;
        if (userdata) {
            verse = *((int *)userdata);
        }
        reset_backlight_struct(target_pct, is_smooth, smooth_step, smooth_wait, verse);
        add_backlight_sn(backlight_interface, 1);
        DDCUTIL_LOOP({
            add_backlight_sn(dinfo->sn, 0);
        });
        m_log("Target pct (smooth %d): %s%.2lf\n", is_smooth, target_pct, verse > 0 ? "+" : (verse < 0 ? "-" : ""));
        int ok = 0;
        receive(NULL, &ok);
        // Returns true if no errors happened
        return sd_bus_reply_method_return(m, "b", ok == 0);
    }
    m_log("Failed to parse parameters: %s\n", strerror(-r));
    return r;
}

static double next_backlight_level(int curr, int max, int idx) {
    double curr_pct = curr / (double)max;
    double target_pct = sc.target_pct;
    if (sc.verse != 0) {
        target_pct = curr_pct + (sc.verse * sc.target_pct);
        /* Sanity checks */
        if (target_pct > 1.0) {
            target_pct = 1.0;
        } else if (target_pct < 0.0) {
            target_pct = 0.0;
        }
    } 
    if (sc.smooth_step > 0) {
        if (target_pct < curr_pct) {
            curr_pct = (curr_pct - sc.smooth_step < target_pct) ? 
            target_pct : curr_pct - sc.smooth_step;
        } else if (target_pct > curr_pct) {
            curr_pct = (curr_pct + sc.smooth_step) > target_pct ? 
            target_pct : curr_pct + sc.smooth_step;
        } else {
            curr_pct = -1.0f;
        }
    } else {
        curr_pct = target_pct;
    }

    if (idx >= 0 && curr_pct == target_pct) {
        sc.d[idx].reached_target = 1;
    }

    return curr_pct;
}

static int set_internal_backlight(int idx) {
    int r = -1;

    struct udev_device *dev = NULL;
    get_udev_device(sc.d[idx].sn, "backlight", NULL, NULL, &dev);
    if (dev) {
        int max = atoi(udev_device_get_sysattr_value(dev, "max_brightness"));
        int curr = atoi(udev_device_get_sysattr_value(dev, "brightness"));
        int value = next_backlight_level(curr, max, idx) * max;
        /* Check if next_backlight_level returned -1 */
        if (value >= 0) {
            char val[10] = {0};
            sprintf(val, "%d", value);
            r = udev_device_set_sysattr_value(dev, "brightness", val);
        }
        udev_device_unref(dev);
    }
    return r;
}

static int set_external_backlight(int idx) {
    int ret = -1;

    DDCUTIL_FUNC(sc.d[idx].sn, {
        const uint16_t max = VALREC_MAX_VAL(valrec);
        const uint16_t curr = VALREC_CUR_VAL(valrec);
        int16_t new_value = next_backlight_level(curr, max, idx) * max;
        int8_t new_sh = new_value >> 8;
        int8_t new_sl = new_value & 0xff;
        if (new_value >= 0 && ddca_set_non_table_vcp_value(dh, br_code, new_sh, new_sl) == 0) {
            ret = 0;
        }
    });
    return ret;
}

/**
 * Backlight pct getter method: for each screen (both internal and external)
 * it founds, it will return a "(serialNumber, current backlight pct)" struct.
 * Note that for internal laptop screen, serialNumber = syspath (eg: intel_backlight)
 */
static int method_getallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    sd_bus_message *reply = NULL;
    const char *backlight_interface = NULL;

    int r = sd_bus_message_read(m, "s", &backlight_interface);
    if (r >= 0) {
        sd_bus_message_new_method_return(m, &reply);
        sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(sd)");

        append_internal_backlight(reply, backlight_interface);
        append_external_backlight(reply, NULL);

        sd_bus_message_close_container(reply);
        r = sd_bus_send(NULL, reply, NULL);
        sd_bus_message_unref(reply);
        sd_bus_message_exit_container(m);
    } else {
        m_log("Failed to parse parameters: %s\n", strerror(-r));
    }
    return r;
}

static void append_backlight(sd_bus_message *reply, const char *name, const double pct) {
    sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "sd");
    sd_bus_message_append(reply, "sd", name, pct);
    sd_bus_message_close_container(reply);
}

static int append_internal_backlight(sd_bus_message *reply, const char *path) {
    int ret = -1;
    struct udev_device *dev = NULL;
    get_udev_device(path, "backlight", NULL, NULL, &dev);

    if (dev) {
        int val = atoi(udev_device_get_sysattr_value(dev, "brightness"));
        int max = atoi(udev_device_get_sysattr_value(dev, "max_brightness"));

        double pct = (double)val / max;
        append_backlight(reply, udev_device_get_sysname(dev), pct);
        udev_device_unref(dev);
        ret = 0;
    }
    return ret;
}

static int append_external_backlight(sd_bus_message *reply, const char *sn) {
    if (sn) {
        DDCUTIL_FUNC(sn, {
            append_backlight(reply, sn, (double)VALREC_CUR_VAL(valrec) / VALREC_MAX_VAL(valrec));
        });
    } else {
        DDCUTIL_LOOP({
            append_backlight(reply, dinfo->sn, (double)VALREC_CUR_VAL(valrec) / VALREC_MAX_VAL(valrec));
        });
    }
    return 0;
}

static int method_raiseallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int verse = 1;
    return method_setallbrightness(m, &verse, ret_error);
}

static int method_lowerallbrightness(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int verse = -1;
    return method_setallbrightness(m, &verse, ret_error);
}
