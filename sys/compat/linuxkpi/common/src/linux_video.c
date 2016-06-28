
#include <linux/export.h>
#include <linux/acpi.h>
#include <linux/backlight.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <acpi/video.h>

static struct work_struct backlight_notify_work;
static bool backlight_notifier_registered;
static struct notifier_block backlight_nb;

static enum acpi_backlight_type acpi_backlight_dmi = acpi_backlight_undef;

extern uint8_t AcpiGbl_OsiData;

static acpi_status
find_video(acpi_handle handle, u32 lvl, void *context, void **rv)
{
	long *cap = context;
	struct pci_dev *dev;
	struct acpi_device *acpi_dev;

	static const struct acpi_device_id video_ids[] = {
		{ACPI_VIDEO_HID, 0},
		{"", 0},
	};
	if (acpi_bus_get_device(handle, &acpi_dev))
		return AE_OK;

	if (!acpi_match_device_ids(acpi_dev, video_ids)) {
		dev = acpi_get_pci_dev(handle);
		if (!dev)
			return AE_OK;
		pci_dev_put(dev);
		*cap |= acpi_is_video_device(handle);
	}
	return AE_OK;
}

static int video_detect_force_vendor(const struct dmi_system_id *d)
{
	acpi_backlight_dmi = acpi_backlight_vendor;
	return 0;
}

static int video_detect_force_video(const struct dmi_system_id *d)
{
	acpi_backlight_dmi = acpi_backlight_video;
	return 0;
}

static int video_detect_force_native(const struct dmi_system_id *d)
{
	acpi_backlight_dmi = acpi_backlight_native;
	return 0;
}

static const struct dmi_system_id video_detect_dmi_table[] = {
	/* On Samsung X360, the BIOS will set a flag (VDRV) if generic
	 * ACPI backlight device is used. This flag will definitively break
	 * the backlight interface (even the vendor interface) untill next
	 * reboot. It's why we should prevent video.ko from being used here
	 * and we can't rely on a later call to acpi_video_unregister().
	 */
	{
	 .callback = video_detect_force_vendor,
	 .ident = "X360",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME, "X360"),
		DMI_MATCH(DMI_BOARD_NAME, "X360"),
		},
	},
	{
	.callback = video_detect_force_vendor,
	.ident = "Asus UL30VT",
	.matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK Computer Inc."),
		DMI_MATCH(DMI_PRODUCT_NAME, "UL30VT"),
		},
	},
	{
	.callback = video_detect_force_vendor,
	.ident = "Asus UL30A",
	.matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK Computer Inc."),
		DMI_MATCH(DMI_PRODUCT_NAME, "UL30A"),
		},
	},

	/*
	 * These models have a working acpi_video backlight control, and using
	 * native backlight causes a regression where backlight does not work
	 * when userspace is not handling brightness key events. Disable
	 * native_backlight on these to fix this:
	 * https://bugzilla.kernel.org/show_bug.cgi?id=81691
	 */
	{
	 .callback = video_detect_force_video,
	 .ident = "ThinkPad T420",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad T420"),
		},
	},
	{
	 .callback = video_detect_force_video,
	 .ident = "ThinkPad T520",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad T520"),
		},
	},
	{
	 .callback = video_detect_force_video,
	 .ident = "ThinkPad X201s",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad X201s"),
		},
	},

	/* The native backlight controls do not work on some older machines */
	{
	 /* https://bugs.freedesktop.org/show_bug.cgi?id=81515 */
	 .callback = video_detect_force_video,
	 .ident = "HP ENVY 15 Notebook",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
		DMI_MATCH(DMI_PRODUCT_NAME, "HP ENVY 15 Notebook PC"),
		},
	},
	{
	 .callback = video_detect_force_video,
	 .ident = "SAMSUNG 870Z5E/880Z5E/680Z5E",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME, "870Z5E/880Z5E/680Z5E"),
		},
	},
	{
	 .callback = video_detect_force_video,
	 .ident = "SAMSUNG 370R4E/370R4V/370R5E/3570RE/370R5V",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME,
			  "370R4E/370R4V/370R5E/3570RE/370R5V"),
		},
	},
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1186097 */
	 .callback = video_detect_force_video,
	 .ident = "SAMSUNG 3570R/370R/470R/450R/510R/4450RV",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME,
			  "3570R/370R/470R/450R/510R/4450RV"),
		},
	},
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1094948 */
	 .callback = video_detect_force_video,
	 .ident = "SAMSUNG 730U3E/740U3E",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME, "730U3E/740U3E"),
		},
	},
	{
	 /* https://bugs.freedesktop.org/show_bug.cgi?id=87286 */
	 .callback = video_detect_force_video,
	 .ident = "SAMSUNG 900X3C/900X3D/900X3E/900X4C/900X4D",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME,
			  "900X3C/900X3D/900X3E/900X4C/900X4D"),
		},
	},
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1272633 */
	 .callback = video_detect_force_video,
	 .ident = "Dell XPS14 L421X",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
		DMI_MATCH(DMI_PRODUCT_NAME, "XPS L421X"),
		},
	},
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1163574 */
	 .callback = video_detect_force_video,
	 .ident = "Dell XPS15 L521X",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
		DMI_MATCH(DMI_PRODUCT_NAME, "XPS L521X"),
		},
	},
	{
	 /* https://bugzilla.kernel.org/show_bug.cgi?id=108971 */
	 .callback = video_detect_force_video,
	 .ident = "SAMSUNG 530U4E/540U4E",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		DMI_MATCH(DMI_PRODUCT_NAME, "530U4E/540U4E"),
		},
	},

	/* Non win8 machines which need native backlight nevertheless */
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1201530 */
	 .callback = video_detect_force_native,
	 .ident = "Lenovo Ideapad S405",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		DMI_MATCH(DMI_BOARD_NAME, "Lenovo IdeaPad S405"),
		},
	},
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1187004 */
	 .callback = video_detect_force_native,
	 .ident = "Lenovo Ideapad Z570",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		DMI_MATCH(DMI_PRODUCT_NAME, "102434U"),
		},
	},
	{
	 /* https://bugzilla.redhat.com/show_bug.cgi?id=1217249 */
	 .callback = video_detect_force_native,
	 .ident = "Apple MacBook Pro 12,1",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
		DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro12,1"),
		},
	},
	{
	 .callback = video_detect_force_native,
	 .ident = "Dell Vostro V131",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
		DMI_MATCH(DMI_PRODUCT_NAME, "Vostro V131"),
		},
	},
	{ },
};


static void
acpi_video_unregister_backlight(void)
{
	/* XXX */
}

/* This uses a workqueue to avoid various locking ordering issues */
static void
acpi_video_backlight_notify_work(struct work_struct *work)
{
	if (acpi_video_get_backlight_type() != acpi_backlight_video)
		acpi_video_unregister_backlight();
}

static int
acpi_video_backlight_notify(struct notifier_block *nb,
				       unsigned long val, void *bd)
{
	struct backlight_device *backlight = bd;

	/* A raw bl registering may change video -> native */
	if (backlight->props.type == BACKLIGHT_RAW &&
	    val == BACKLIGHT_REGISTERED)
		schedule_work(&backlight_notify_work);

	return NOTIFY_OK;
}

bool
backlight_device_registered(enum backlight_type type)
{
	/* XXX FIXME */
	return (FALSE);
}


enum acpi_backlight_type
acpi_video_get_backlight_type(void)
{
	static DEFINE_MUTEX(init_mutex);
	static bool init_done;
	static long video_caps;

	/* Parse cmdline, dmi and acpi only once */
	mutex_lock(&init_mutex);
	if (!init_done) {
		dmi_check_system(video_detect_dmi_table);
		acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				    ACPI_UINT32_MAX, find_video, NULL,
				    &video_caps, NULL);
		INIT_WORK(&backlight_notify_work,
			  acpi_video_backlight_notify_work);
		backlight_nb.notifier_call = acpi_video_backlight_notify;
		backlight_nb.priority = 0;
		if (backlight_register_notifier(&backlight_nb) == 0)
			backlight_notifier_registered = true;
		init_done = true;
	}
	mutex_unlock(&init_mutex);

	if (acpi_backlight_dmi != acpi_backlight_undef)
		return acpi_backlight_dmi;

	if (!(video_caps & ACPI_VIDEO_BACKLIGHT))
		return acpi_backlight_vendor;

	if ((AcpiGbl_OsiData >= ACPI_OSI_WIN_8) && backlight_device_registered(BACKLIGHT_RAW))
		return acpi_backlight_native;

	return acpi_backlight_video;
}