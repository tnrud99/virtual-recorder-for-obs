#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QDockWidget>
#include <QList>
#include <QMainWindow>
#include <QObject>
#include <QTimer>
#include "dock-widget.hpp"
#include "youtube-controller.hpp"
#include "scene-manager.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("virtuedit-obs", "en-US")

static VirtuEditDock *dock = nullptr;
static YouTubeController *ytController = nullptr;
static SceneManager *sceneMgr = nullptr;
static QMainWindow *mainWindow = nullptr;
static QDockWidget *dockWrapper = nullptr;
static QAction *toolsMenuAction = nullptr;
static int startupRaiseGeneration = 0;
static const char *BROWSER_SOURCE_NAME = "Virtual Recorder Player Controls";
static const char *PREVIOUS_BROWSER_SOURCE_NAME =
	"Control YouTube Here While Recording";
static const char *OLDER_PREVIOUS_BROWSER_SOURCE_NAME_2 =
	"Watch YouTube Here for Sync";
static const char *OLDER_PREVIOUS_BROWSER_SOURCE_NAME =
	"Virtual Recorder Synced Player";
static const char *OLDER_BROWSER_SOURCE_NAME_VR =
	"Virtual Recorder YouTube Player";
static const char *LEGACY_BROWSER_SOURCE_NAME = "VirtuEdit YouTube Player";
static const char *OLDER_BROWSER_SOURCE_NAME = "VirtuEdit - Watch Video";
static const char *OLDEST_BROWSER_SOURCE_NAME = "VirtuEdit-YouTube";
static constexpr int DOCK_MIN_WIDTH = 360;
static constexpr int DOCK_DEFAULT_WIDTH = 400;

static QDockWidget *find_dock_wrapper(QWidget *widget)
{
	for (QWidget *cur = widget; cur; cur = cur->parentWidget()) {
		if (auto *dockWidget = qobject_cast<QDockWidget *>(cur))
			return dockWidget;
	}
	return nullptr;
}

static QDockWidget *active_dock_wrapper()
{
	return dockWrapper ? dockWrapper : find_dock_wrapper(dock);
}

static bool is_docked_visible_on_right(QDockWidget *wrapper)
{
	if (!wrapper || !mainWindow)
		return false;
	if (wrapper->isFloating() || !wrapper->isVisible())
		return false;
	return mainWindow->dockWidgetArea(wrapper) == Qt::RightDockWidgetArea;
}

// Startup only: recover a closed/undocked dock onto the right. Skip
// addDockWidget when already visible there to avoid flicker and tab reshuffles.
// Default width is applied once; later width changes stick. The persisted flag
// is still named initial_dock_float_shown for compatibility.
static void ensure_virtual_recorder_visible_on_right()
{
	if (!dockWrapper || !mainWindow)
		return;

	if (!is_docked_visible_on_right(dockWrapper)) {
		dockWrapper->setFloating(false);
		mainWindow->addDockWidget(Qt::RightDockWidgetArea, dockWrapper);
	}

	if (sceneMgr && !sceneMgr->isInitialDockPlacementShown()) {
		mainWindow->resizeDocks(QList<QDockWidget *>{dockWrapper},
					QList<int>{DOCK_DEFAULT_WIDTH},
					Qt::Horizontal);
		sceneMgr->setInitialDockPlacementShown(true);
		blog(LOG_INFO,
		     "[VirtuEdit] Docked Virtual Recorder on the right (default width %d)",
		     DOCK_DEFAULT_WIDTH);
	}

	dockWrapper->show();
	dockWrapper->raise();
}

// Session recovery (Tools / scene collection): show without moving the dock.
static void show_virtual_recorder_dock()
{
	QDockWidget *wrapper = active_dock_wrapper();
	if (!wrapper)
		return;

	if (QAction *toggle = wrapper->toggleViewAction())
		toggle->setChecked(true);
	wrapper->setVisible(true);
	wrapper->show();
	wrapper->raise();
}

static void raise_virtual_recorder_dock()
{
	if (!dock)
		return;

	if (mainWindow) {
		mainWindow->show();
		mainWindow->raise();
		mainWindow->activateWindow();
	}

	show_virtual_recorder_dock();

	dock->show();
	dock->raise();
	dock->setFocus(Qt::OtherFocusReason);
}

static void cancel_virtual_recorder_startup_raise()
{
	startupRaiseGeneration++;
}

static void schedule_virtual_recorder_startup_raise()
{
	const int generation = ++startupRaiseGeneration;
	const int delays[] = {0, 750, 2000, 3500};
	for (int delay : delays) {
		QTimer::singleShot(delay, [generation]() {
			if (generation != startupRaiseGeneration)
				return;
			ensure_virtual_recorder_visible_on_right();
			raise_virtual_recorder_dock();
		});
	}
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	(void)data;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		schedule_virtual_recorder_startup_raise();
		[[fallthrough]];
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED: {
		show_virtual_recorder_dock();
		if (sceneMgr)
			sceneMgr->restorePendingAudioTracks();
		// Scenes are now loaded — restore our sources from config
		if (sceneMgr && !sceneMgr->isSetupDone()) {
			if (sceneMgr->restoreFromConfig()) {
				blog(LOG_INFO,
				     "[VirtuEdit] Previous session restored");
				// Also restore browser source reference
				if (ytController) {
					obs_source_t *bSrc =
						ytController->getBrowserSource();
					if (!bSrc) {
						obs_source_t *restored =
							sceneMgr->getBrowserSource();
						if (restored) {
							ytController->setBrowserSourceFromScene(
								restored);
							bSrc = ytController->getBrowserSource();
						}
					}
					if (!bSrc) {
						// Try to find it in the scene
						obs_scene_t *sc =
							sceneMgr->getScene();
						if (sc) {
							obs_sceneitem_t *item = nullptr;
							const char *names[] = {
								BROWSER_SOURCE_NAME,
								PREVIOUS_BROWSER_SOURCE_NAME,
								OLDER_PREVIOUS_BROWSER_SOURCE_NAME_2,
								OLDER_PREVIOUS_BROWSER_SOURCE_NAME,
								OLDER_BROWSER_SOURCE_NAME_VR,
								LEGACY_BROWSER_SOURCE_NAME,
								OLDER_BROWSER_SOURCE_NAME,
								OLDEST_BROWSER_SOURCE_NAME,
							};
							for (const char *name : names) {
								item = obs_scene_find_source(sc, name);
								if (item)
									break;
							}
							if (item)
								ytController->setBrowserSourceFromScene(
									obs_sceneitem_get_source(
										item));
						}
					}
				}
				if (dock)
					dock->onSessionRestored();
			}
		}
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (dock)
			dock->onFrontendSceneChanged();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		char *path = obs_frontend_get_last_recording();
		if (path) {
			blog(LOG_INFO,
			     "[VirtuEdit] Recording saved: %s", path);
			if (dock)
				dock->onCompositeRecordingReady(path);
			bfree(path);
		} else {
			blog(LOG_WARNING,
			     "[VirtuEdit] Recording stopped but OBS did not report a path");
			if (dock)
				dock->onCompositeRecordingReady("");
		}
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (dock)
			dock->onCompositeRecordingStarted();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	ytController = new YouTubeController();
	if (!ytController->init()) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Failed to initialize WebSocket server");
		delete ytController;
		ytController = nullptr;
		return false;
	}

	sceneMgr = new SceneManager();

	obs_frontend_push_ui_translation(obs_module_get_string);

	mainWindow = (QMainWindow *)obs_frontend_get_main_window();

	dock = new VirtuEditDock(mainWindow, ytController, sceneMgr);
	dockWrapper = new QDockWidget(
		QString::fromUtf8(obs_module_text("VirtuEditDock")), mainWindow);
	dockWrapper->setObjectName(QStringLiteral("virtuedit_dock"));
	dockWrapper->setWidget(dock);
	dockWrapper->setAllowedAreas(Qt::LeftDockWidgetArea |
				     Qt::RightDockWidgetArea);
	dockWrapper->setMinimumWidth(DOCK_MIN_WIDTH);
	if (!obs_frontend_add_custom_qdock("virtuedit_dock", dockWrapper)) {
		blog(LOG_WARNING,
		     "[VirtuEdit] Failed to add custom dock; falling back to standard dock");
		dockWrapper->setWidget(nullptr);
		dockWrapper->deleteLater();
		dockWrapper = nullptr;
		obs_frontend_add_dock_by_id("virtuedit_dock",
					     obs_module_text("VirtuEditDock"),
					     dock);
	}

	toolsMenuAction = reinterpret_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(
			obs_module_text("OpenVirtuEditDock")));
	if (toolsMenuAction)
		QObject::connect(toolsMenuAction, &QAction::triggered,
				 []() { raise_virtual_recorder_dock(); });

	schedule_virtual_recorder_startup_raise();

	obs_frontend_pop_ui_translation();

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	blog(LOG_INFO, "[VirtuEdit] Plugin loaded (v%s, ws port %d)", VIRTUEDIT_VERSION,
	     ytController->getWsPort());
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	cancel_virtual_recorder_startup_raise();

	// Stop background WebSocket delivery before destroying the dock callbacks.
	if (ytController)
		ytController->shutdown();

	if (dock) {
		obs_frontend_remove_dock("virtuedit_dock");
		if (dockWrapper) {
			delete dockWrapper;
			dockWrapper = nullptr;
		}
		dock = nullptr;
	}
	if (toolsMenuAction) {
		delete toolsMenuAction;
		toolsMenuAction = nullptr;
	}
	mainWindow = nullptr;
	if (ytController) {
		delete ytController;
		ytController = nullptr;
	}
	if (sceneMgr) {
		delete sceneMgr;
		sceneMgr = nullptr;
	}
	blog(LOG_INFO, "[VirtuEdit] Plugin unloaded");
}
