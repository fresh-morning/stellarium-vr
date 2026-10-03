/*
 * Copyright (C) 2017 Guillaume Chereau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "OpenXR.hpp"
#include "StelActionMgr.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelMainView.hpp"
#include "LandscapeMgr.hpp"
#include "StelGuiBase.hpp"
#include "StelLocaleMgr.hpp"
#include "StelModuleMgr.hpp"
#include "StelMovementMgr.hpp"
#include "StelObjectModule.hpp"
#include "StelSkyDrawer.hpp"
#include "StelObjectMgr.hpp"
#include "StelOpenGL.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelTranslator.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QGraphicsPixmapItem>
#include <QGuiApplication>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QSettings>
#include <QtMath>
#include <QVector2D>
#include <QPainter>
#include <QPolygonF>
#include <QQuaternion>
#include <QVector3D>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

// Last: X11 defines macros (None, Bool, Status...) that break Qt headers.
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <openxr/openxr_platform.h>

namespace
{
// Where the GUI panel floats, in meters: ahead of the head and a little below
// the eyes, facing it. Calibration knobs, untried on a headset.
constexpr float panelDistance = 1.5f;
constexpr float panelDrop = 0.1f;
// The panel is as big as it takes to show the GUI font this tall. Meta asks for
// about 1° to read comfortably (18 dmm), Microsoft for 0.6-0.8°. Too wide a GUI
// shrinks the text instead of going round the head.
constexpr double textDegrees = 1.0;
constexpr double maxPanelDegrees = 90.;
// The GUI is drawn this much finer than the window, as the headset shows about
// 20 pixels per degree; one window pixel would be 13 at 1° text.
constexpr qreal panelScale = 1.5;
// The menu is this wide, just over the left hand.
constexpr float handMenuWidth = 0.26f;
// Held like a painter's palette, as Tilt Brush's: fixed to the left
// controller's aim pose, the same on every controller as its grip pose is
// not, and turning with the wrist. The hand is held below the eyes, pointing
// ahead and down, which already turns the panel's face up towards them; it
// leans back only a little more, its bottom edge this far above and ahead of the controller, clear of the
// other hand (Microsoft puts its hand menus about 13 cm over the hand). It
// follows the hand through a little smoothing, more while the pointer is on
// it, so the tile aimed at holds still (the part Microsoft's hand menus
// freeze as the other hand comes to them).
constexpr float handMenuTiltDegrees = 10.f;
// With the elbow bent the left hand points about this far in, across the
// body, which turns the panel's face out to the left: it swings back as far,
// like a door about the controller's up, counterclockwise seen from above.
constexpr float handMenuSwingDegrees = 30.f;
constexpr float handMenuAbove = 0.05f, handMenuAhead = 0.05f;
constexpr double handMenuFollowSeconds = 0.03, handMenuAimedFollowSeconds = 0.15;
// Turned this far from the face, it hides rather than show its back.
constexpr float handMenuFacingAway = 0.15f;
// A flick of that hand's stick turns it to the next category or page.
constexpr float flickOn = 0.7f, flickOff = 0.3f;
constexpr int leftHand = 0, rightHand = 1;

// The object's card floats in the sky just beside it, far enough off to be
// seen at about the depth of the stars, its labels as big as the menu's.
constexpr float cardDistance = 20.f;
const double cardDegrees = OpenXRMenu::size(OpenXRMenu::Card).width() / 30. * 1.2, cardGapDegrees = 3.;

// The time bar, low in the view, shown for a while after the time changes
// other than by running on.
constexpr float timeBarWidth = 0.5f;
constexpr XrVector3f timeBarPlace{0.f, -0.3f, -1.f};
constexpr qint64 timeBarShowNs = 2000000000;

// "Show me" points the way to the object picked, from a meter ahead, until
// the head turns to within this of it.
constexpr float arrowDistance = 1.f, arrowSize = 0.12f;
constexpr double arrowDoneDegrees = 8.;

// The right stick, pointed at the sky: left and right scrub time, up and down
// zoom the loupe.
constexpr float stickDeadZone = 0.2f;
constexpr double scrubHoursPerSecond = 2.;
constexpr double zoomPerSecond = 1.5;
constexpr float laserThickness = 0.004f;
// The sky itself is never magnified: zoomed, it would lose the horizon and
// the stars around, the head's turns would hardly move it and the hand's
// tremor would shake it. The loupe, a round magnified view of the object
// picked, floats in the sky on its other side from the card, this many
// degrees across. Y opens it with the object filling a quarter of its field,
// a star with a degree of sky around; the stick narrows or widens that.
constexpr double loupeDegrees = 20., loupeFovPerRadius = 8., starLoupeFov = 1., minLoupeFov = 0.25, maxLoupeFov = 8.;
constexpr int loupePixels = 800;
const float loupeMeters = static_cast<float>(2. * cardDistance * std::tan(0.5 * loupeDegrees / M_180_PI));

float deadZone(float v)
{
	return std::fabs(v) < stickDeadZone ? 0.f : (v - std::copysign(stickDeadZone, v)) / (1.f - stickDeadZone);
}

// Hysteresis for the trigger, so one resting half way doesn't chatter.
constexpr float triggerPress = 0.7f, triggerRelease = 0.4f;

constexpr int64_t formatSrgb8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8
constexpr int64_t formatRgba8 = 0x8058;       // GL_RGBA8

// OpenXR has +x right, +y up and -z forward; Stellarium's AltAz frame has z up.
// yaw turns the result around the zenith.
QVector3D fromAltAz(Vec3d altAz, double yaw)
{
	// toAltAz() undone, which is its own inverse.
	altAz.normalize();
	const double c = std::cos(yaw), s = std::sin(yaw);
	return QVector3D(float(c * altAz[0] + s * altAz[1]), float(altAz[2]), float(s * altAz[0] - c * altAz[1]));
}

// yaw turns the result around the zenith.
Vec3d toAltAz(const XrQuaternionf& q, const QVector3D& v, double yaw)
{
	const QVector3D r = QQuaternion(q.w, q.x, q.y, q.z).rotatedVector(v);
	const double c = std::cos(yaw), s = std::sin(yaw);
	return Vec3d(c*r.x() + s*r.z(), s*r.x() - c*r.z(), r.y());
}

// Same layout as StelCore::lookAtJ2000(), from the eye's own axes.
Mat4d modelView(const XrQuaternionf& q, double yaw)
{
	const Vec3d s = toAltAz(q, {1, 0, 0}, yaw);
	const Vec3d u = toAltAz(q, {0, 1, 0}, yaw);
	const Vec3d f = toAltAz(q, {0, 0, -1}, yaw);
	return Mat4d(s[0], u[0], -f[0], 0.,
		     s[1], u[1], -f[1], 0.,
		     s[2], u[2], -f[2], 0.,
		     0., 0., 0., 1.);
}

double verticalFov(const XrFovf& fov)
{
	return 2. * std::atan(0.5 * (std::tan(fov.angleUp) + std::tan(-fov.angleDown))) * M_180_PI;
}

// A perspective projection with the eye's asymmetric field of view: the
// vertical one sets the scale, widthStretch the horizontal one, and the
// center offset where straight ahead falls in the image.
StelProjector::StelProjectorParams eyeParams(StelProjector::StelProjectorParams p, const XrFovf& fov, int width, int height)
{
	const double l = std::tan(-fov.angleLeft), r = std::tan(fov.angleRight);
	const double u = std::tan(fov.angleUp), d = std::tan(-fov.angleDown);
	p.devicePixelsPerPixel = 1.;
	p.viewportXywh.set(0, 0, width, height);
	p.viewportCenterOffset.set(l/(l+r) - 0.5, d/(u+d) - 0.5);
	p.viewportCenter.set(width*l/(l+r), height*d/(u+d));
	p.viewportFovDiameter = height;
	p.fov = static_cast<float>(verticalFov(fov));
	p.widthStretch = (width/(l+r)) / (height/(u+d));
	p.flipHorz = p.flipVert = false;
	p.maskType = StelProjector::MaskNone;
	return p;
}

// A press there reaches a button or a dialog, through what lies over them
// (the info text does), as Qt passes it on. Elsewhere on the GUI (bar
// backgrounds, labels) Stellarium would hand it on to the sky, picking with the
// window's projection instead of the controller's ray.
const QGraphicsItem* clickable(const QPointF& pos)
{
	for (const QGraphicsItem* item : StelMainView::getInstance().scene()->items(pos))
		if (item->type() == QGraphicsPixmapItem::Type || item->type() == QGraphicsProxyWidget::Type)
			return item;
	return nullptr;
}

// What the trigger picks in the sky along a ray: StelObjectMgr::cleverFind()'s
// rule, the nearest weighed by its brightness, but measured in the headset's
// pixels instead of the window's, which made the search twice as wide. The
// thousands of satellites only count right under the pointer, or they would
// take most picks. In the loupe, all of these are as many times smaller in
// the sky as it magnifies, staying the same around the dot. Only what was
// drawn counts: the loupe draws fainter stars than the eyes.
constexpr double pickRadius = 1.5, pickPixel = 0.06, satellitePickRadius = 0.5; // degrees
StelObjectP pickAlong(const StelCore* core, Vec3d j2000, double magnification, float drawnLimit)
{
	j2000.normalize();
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const float limitMagnitude = drawer->getFlagStarMagnitudeLimit() ? static_cast<float>(drawer->getCustomStarMagnitudeLimit())
									 : drawnLimit;
	StelObjectP best;
	double bestScore = 1e9;
	for (StelModule* module : StelApp::getInstance().getModuleMgr().getAllModules())
	{
		const StelObjectModule* objects = dynamic_cast<const StelObjectModule*>(module);
		if (!objects)
			continue;
		for (const StelObjectP& object : objects->searchAround(j2000, pickRadius / magnification, core))
		{
			const float priority = object->getSelectPriority(core);
			if (priority > limitMagnitude)
				continue;
			Vec3d position = object->getJ2000EquatorialPos(core);
			position.normalize();
			const double degrees = std::acos(qBound(-1., position.dot(j2000), 1.)) * M_180_PI;
			if (object->getType() == QLatin1String("Satellite") && degrees > satellitePickRadius / magnification)
				continue;
			const double score = degrees * magnification / pickPixel + priority;
			if (score < bestScore)
			{
				bestScore = score;
				best = object;
			}
		}
	}
	return best;
}

QVector3D toQt(const XrVector3f& v) { return QVector3D(v.x, v.y, v.z); }
QQuaternion toQt(const XrQuaternionf& q) { return QQuaternion(q.w, q.x, q.y, q.z); }
XrVector3f toXr(const QVector3D& v) { return {v.x(), v.y(), v.z()}; }
XrQuaternionf toXr(const QQuaternion& q) { return {q.x(), q.y(), q.z(), q.scalar()}; }

// Facing the head from where, and upright to it as the labels in the sky are.
QQuaternion facingHead(const QVector3D& where, const XrPosef& head)
{
	const QVector3D up = toQt(head.orientation).rotatedVector(QVector3D(0.f, 1.f, 0.f));
	return QQuaternion::fromDirection(toQt(head.position) - where, up);
}

void postMouse(QEvent::Type type, const QPointF& pos, Qt::MouseButton button, Qt::MouseButtons buttons)
{
	QWidget* viewport = StelMainView::getInstance().viewport();
	QCoreApplication::postEvent(viewport, new QMouseEvent(type, pos, viewport->mapToGlobal(pos), button, buttons, Qt::NoModifier));
}

// The pointer's red, as a laser's.
const QColor laserColor(255, 40, 30);

// A red dot, outlined to stand out on a light GUI.
QImage cursorImage()
{
	QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QColor(0, 0, 0, 160), 6));
	painter.setBrush(laserColor);
	painter.drawEllipse(QPointF(32, 32), 20, 20);
	return image;
}

// In the sky: the dot alone, about a third of a degree across.
constexpr int skyCursorPixels = 32, skyDotRadius = 12;
constexpr double skyDotDegrees = 0.35;
QImage skyCursorImage()
{
	QImage image(skyCursorPixels, skyCursorPixels, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(laserColor);
	painter.drawEllipse(QPointF(skyCursorPixels / 2, skyCursorPixels / 2), skyDotRadius, skyDotRadius);
	return image;
}

// An arrow pointing right, to be turned in the view towards the object.
QImage arrowImage()
{
	QImage image(128, 128, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	const QPolygonF arrow({{16, 48}, {68, 48}, {68, 20}, {116, 64}, {68, 108}, {68, 80}, {16, 80}});
	painter.setPen(QPen(QColor(0, 0, 0, 160), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.setBrush(QColor(170, 210, 255));
	painter.drawPolygon(arrow);
	return image;
}

// A beam fading out from the controller; the image runs from its start to its end.
QImage laserImage()
{
	QImage image(256, 16, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	QLinearGradient fade(0, 0, 256, 0);
	fade.setColorAt(0., QColor(255, 40, 30, 220));
	fade.setColorAt(1., QColor(255, 40, 30, 0));
	painter.setPen(Qt::NoPen);
	painter.setBrush(fade);
	painter.drawRoundedRect(QRectF(0, 4, 256, 8), 4, 4);
	return image;
}

// The loupe's view is cut to a circle this big, in its own half-widths.
constexpr float eyepieceRadius = 0.94f;

// A thin rim around the round view, to set it off from the sky behind.
QImage loupeRimImage()
{
	QImage image(loupePixels, loupePixels, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	const double radius = loupePixels / 2. * eyepieceRadius;
	painter.setBrush(Qt::NoBrush);
	painter.setPen(QPen(QColor(20, 24, 34), 8));
	painter.drawEllipse(QPointF(loupePixels / 2., loupePixels / 2.), radius + 3., radius + 3.);
	painter.setPen(QPen(QColor(110, 120, 140), 3));
	painter.drawEllipse(QPointF(loupePixels / 2., loupePixels / 2.), radius, radius);
	return image;
}

const char* stateName(XrSessionState state)
{
	static const char* const names[] = {"UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
					    "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
	return state >= 0 && state <= XR_SESSION_STATE_EXITING ? names[state] : "?";
}
}

StelModule* OpenXRStelPluginInterface::getStelModule() const
{
	return new OpenXR();
}

StelPluginInfo OpenXRStelPluginInterface::getPluginInfo() const
{
	StelPluginInfo info;
	info.id = "OpenXR";
	info.displayedName = N_("OpenXR");
	info.authors = "Guillaume Chereau";
	info.contact = "guillaume@noctua-software.com";
	info.description = N_("Shows the sky in a VR headset through OpenXR");
	info.version = OPENXR_PLUGIN_VERSION;
	info.startByDefault = true;
	return info;
}

OpenXR::OpenXR()
{
	setObjectName("OpenXR");
	StelActionMgr *actionsMgr = StelApp::getInstance().getStelActionManager();
	actionsMgr->findAction("actionGoto_Selected_Object")->setShortcut("");
	actionsMgr->addAction("actionOpenXR_recenter", "OpenXR", N_("Recenter the headset view"), this, "recenter()", "Space");
}

OpenXR::~OpenXR()
{
}

void OpenXR::init()
{
	// The session needs the GL context current, so it starts with the first frame.
	StelApp::getInstance().setDrawOverride([this] { drawFrame(); });

	menu.recenter = [this] { recenter(); };
	menu.openGui = [this] {
		panelVisible = true;
		panelPlacePending = true;
		panelTimer.start();
	};
	menu.guiOpen = [this] { return panelVisible; };
	menu.showMe = [this] { arrowPending = true; };

	frameTimer.setTimerType(Qt::PreciseTimer);
	frameTimer.setInterval(0);
	connect(&frameTimer, &QTimer::timeout, this, [] { StelMainView::getInstance().runFrame(); });

	panelTimer.setSingleShot(true);
	connect(&panelTimer, &QTimer::timeout, this, &OpenXR::renderPanel);

}

void OpenXR::deinit()
{
	StelApp::getInstance().setDrawOverride(nullptr);
	frameTimer.stop();
	StelMainView::getInstance().setFramesRunExternally(false);
	// Destroys the session, spaces and swapchains with it.
	if (instance)
		xrDestroyInstance(instance);
	instance = XR_NULL_HANDLE;
}

void OpenXR::recenter()
{
	recenterPending = true;
	panelPlacePending = true;
}

bool OpenXR::ok(XrResult result, const char* what) const
{
	if (XR_SUCCEEDED(result))
		return true;
	char name[XR_MAX_RESULT_STRING_SIZE] = "";
	if (instance)
		xrResultToString(instance, result, name);
	qWarning().nospace() << "OpenXR: " << what << " failed: " << (name[0] ? name : QByteArray::number(result).constData());
	return false;
}

bool OpenXR::start()
{
	// Not in the Khronos registry, so enabled by name where the runtime has it.
	const char* const frameControllerName = "XR_VALVE_frame_controller_interaction";
	uint32_t extensionCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
	std::vector<XrExtensionProperties> available(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, available.data());
	frameControllerExtension = std::any_of(available.begin(), available.end(), [&](const XrExtensionProperties& e) {
		return qstrcmp(e.extensionName, frameControllerName) == 0;
	});
	std::vector<const char*> extensions = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
	if (frameControllerExtension)
		extensions.push_back(frameControllerName);
	XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
	qstrncpy(instanceInfo.applicationInfo.applicationName, "Stellarium", XR_MAX_APPLICATION_NAME_SIZE);
	qstrncpy(instanceInfo.applicationInfo.engineName, "Stellarium", XR_MAX_ENGINE_NAME_SIZE);
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instanceInfo.enabledExtensionNames = extensions.data();
	if (!ok(xrCreateInstance(&instanceInfo, &instance), "xrCreateInstance"))
		return false;

	XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
	if (ok(xrGetInstanceProperties(instance, &properties), "xrGetInstanceProperties"))
		qInfo() << "OpenXR: runtime" << properties.runtimeName;

	XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	if (!ok(xrGetSystem(instance, &systemInfo, &system), "xrGetSystem"))
		return false;

	// The spec wants this asked before the session, whatever the answer.
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
	if (!ok(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
				      reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)), "xrGetInstanceProcAddr")
	    || !ok(getRequirements(instance, system, &requirements), "xrGetOpenGLGraphicsRequirementsKHR"))
		return false;

	Display* display = glXGetCurrentDisplay();
	GLXContext context = glXGetCurrentContext();
	if (!display || !context)
	{
		qWarning() << "OpenXR: the OpenGL context is not a GLX one; run under X11 or Xwayland with QT_XCB_GL_INTEGRATION=xcb_glx";
		return false;
	}
	XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
	binding.xDisplay = display;
	binding.glxContext = context;
	binding.glxDrawable = glXGetCurrentDrawable();
	int configId = 0, configCount = 0;
	glXQueryContext(display, context, GLX_FBCONFIG_ID, &configId);
	const int configAttributes[] = {GLX_FBCONFIG_ID, configId, None};
	GLXFBConfig* configs = glXChooseFBConfig(display, DefaultScreen(display), configAttributes, &configCount);
	if (configCount > 0)
	{
		binding.glxFBConfig = configs[0];
		if (XVisualInfo* visual = glXGetVisualFromFBConfig(display, configs[0]))
		{
			binding.visualid = static_cast<uint32_t>(visual->visualid);
			XFree(visual);
		}
	}
	if (configs)
		XFree(configs);

	XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &binding;
	sessionInfo.systemId = system;
	if (!ok(xrCreateSession(instance, &sessionInfo, &session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!ok(xrCreateReferenceSpace(session, &spaceInfo, &localSpace), "xrCreateReferenceSpace(LOCAL)"))
		return false;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!ok(xrCreateReferenceSpace(session, &spaceInfo, &viewSpace), "xrCreateReferenceSpace(VIEW)"))
		return false;
	if (!createActions())
		return false;

	// Stellarium writes display-ready colors: into an sRGB image with
	// GL_FRAMEBUFFER_SRGB off they land unchanged, and the compositor reads
	// them as sRGB. A linear image would show them too bright.
	uint32_t count = 0;
	if (!ok(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "xrEnumerateSwapchainFormats") || count == 0)
		return false;
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(session, count, &count, formats.data());
	format = formats[0];
	for (int64_t wanted : {formatSrgb8Alpha8, formatRgba8})
	{
		if (std::find(formats.begin(), formats.end(), wanted) != formats.end())
		{
			format = wanted;
			break;
		}
	}

	XrViewConfigurationView configViews[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	if (!ok(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, configViews),
		"xrEnumerateViewConfigurationViews"))
		return false;

	for (int i = 0; i < 2; ++i)
		if (!createEye(eyes[i], static_cast<int>(configViews[i].recommendedImageRectWidth),
			       static_cast<int>(configViews[i].recommendedImageRectHeight)))
			return false;
	if (!createEye(loupe, loupePixels, loupePixels))
		return false;

	const QImage ring = cursorImage(), laser = laserImage(), arrow = arrowImage(), sky = skyCursorImage(), rim = loupeRimImage();
	if (!createQuad(cursor, ring.width(), ring.height(), false) || !createQuad(skyCursor, sky.width(), sky.height(), false)
	    || !createQuad(laserQuad, laser.width(), laser.height(), false) || !createQuad(arrowQuad, arrow.width(), arrow.height(), false)
	    || !createQuad(loupeRim, rim.width(), rim.height(), false))
		return false;
	uploadQuad(cursor, ring);
	uploadQuad(skyCursor, sky);
	uploadQuad(laserQuad, laser);
	uploadQuad(arrowQuad, arrow);
	uploadQuad(loupeRim, rim);

	sharedView = qgetenv("STELLARIUM_XR_VIEWS") != "eyes";
	const QByteArray syncName = qgetenv("STELLARIUM_XR_SYNC");
	sync = syncName == "none" ? Sync::Off : syncName == "flush" ? Sync::Flush : Sync::Finish;
	// For tests without controllers: picks that object and opens the loupe on
	// it, as Y would, turned to face it, as the simulator's headset does not turn.
	const QString loupeTest = QString::fromUtf8(qgetenv("STELLARIUM_XR_LOUPE"));
	if (!loupeTest.isEmpty())
		QMetaObject::invokeMethod(this, [this, loupeTest] {
			StelObjectMgr* objects = GETSTELMODULE(StelObjectMgr);
			if (!objects->findAndSelect(loupeTest))
				return;
			StelCore* core = StelApp::getInstance().getCore();
			core->getMovementMgr()->setViewDirectionJ2000(objects->getSelectedObject().first()->getJ2000EquatorialPos(core));
			recenter();
			press(Y, rightHand);
		}, Qt::QueuedConnection);
	qInfo().nospace() << "OpenXR: before handing images over: " << (sync == Sync::Off ? "no sync" : sync == Sync::Flush ? "glFlush" : "glFinish");
	qInfo().nospace() << "OpenXR: session created, " << eyes[0].width << "x" << eyes[0].height
			  << " per eye, swapchain format 0x" << Qt::hex << format;
	return true;
}

bool OpenXR::createSharedView(const XrView* views)
{
	const XrFovf& a = views[0].fov, &b = views[1].fov;
	skyFov = {qMin(a.angleLeft, b.angleLeft), qMax(a.angleRight, b.angleRight),
		  qMax(a.angleUp, b.angleUp), qMin(a.angleDown, b.angleDown)};
	// As many pixels per unit of tangent as an eye has.
	const auto span = [](float from, float to) { return std::tan(to) - std::tan(from); };
	const double perX = eyes[0].width / span(a.angleLeft, a.angleRight), perY = eyes[0].height / span(a.angleDown, a.angleUp);
	const int width = qCeil(perX * span(skyFov.angleLeft, skyFov.angleRight));
	const int height = qCeil(perY * span(skyFov.angleDown, skyFov.angleUp));
	if (!createEye(sky, width, height))
		return false;
	qInfo().nospace() << "OpenXR: one view for both eyes, " << width << "x" << height << ", "
			  << qRound(qRadiansToDegrees(skyFov.angleRight - skyFov.angleLeft)) << "° by "
			  << qRound(qRadiansToDegrees(skyFov.angleUp - skyFov.angleDown)) << "°";
	return true;
}

bool OpenXR::createEye(Eye& e, int width, int height)
{
	e.width = width;
	e.height = height;
	XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT
				   | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swapchainInfo.format = format;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = static_cast<uint32_t>(width);
	swapchainInfo.height = static_cast<uint32_t>(height);
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;
	if (!ok(xrCreateSwapchain(session, &swapchainInfo, &e.swapchain), "xrCreateSwapchain"))
		return false;
	uint32_t count = 0;
	xrEnumerateSwapchainImages(e.swapchain, 0, &count, nullptr);
	std::vector<XrSwapchainImageOpenGLKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
	if (!ok(xrEnumerateSwapchainImages(e.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
		"xrEnumerateSwapchainImages"))
		return false;
	for (const auto& image : images)
		e.images.push_back(image.image);

	// Low graphics mode draws straight into the view, which needs these.
	QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
	GLint previousFbo = 0;
	gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
	gl->glGenRenderbuffers(1, &e.depthStencil);
	gl->glBindRenderbuffer(GL_RENDERBUFFER, e.depthStencil);
	gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	gl->glGenFramebuffers(1, &e.fbo);
	gl->glBindFramebuffer(GL_FRAMEBUFFER, e.fbo);
	gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, e.depthStencil);
	gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
	return true;
}

void OpenXR::stop()
{
	// On a headset the window is nobody's: a session ended is the app closed.
	if (running)
		QCoreApplication::quit();
	if (instance)
		xrDestroyInstance(instance);
	instance = XR_NULL_HANDLE;
	session = XR_NULL_HANDLE;
	localSpace = viewSpace = XR_NULL_HANDLE;
	QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
	for (Eye* e : {&eyes[0], &eyes[1], &sky, &loupe})
	{
		gl->glDeleteFramebuffers(1, &e->fbo);
		gl->glDeleteRenderbuffers(1, &e->depthStencil);
		*e = Eye();
	}
	if (quadVao)
	{
		gl->glDeleteVertexArrays(1, &quadVao);
		gl->glDeleteBuffers(1, &quadVbo);
		quadVao = quadVbo = 0;
	}
	eyepieceProgram.reset();
	if (gpuQueries[0])
		gl->glDeleteQueries(3, gpuQueries);
	gpuQueries[0] = gpuQueries[1] = gpuQueries[2] = 0;
	gpuQueryPending[0] = gpuQueryPending[1] = gpuQueryPending[2] = false;
	actionSet = XR_NULL_HANDLE;
	aimSpaces[0] = aimSpaces[1] = XR_NULL_HANDLE;
	panel = cursor = skyCursor = handMenuQuad = cardQuad = timeBarQuad = laserQuad = arrowQuad = loupeRim = Quad();
	cursorVisible = false;
	setRunning(false);
	failed = true;
	qInfo() << "OpenXR: stopped, the window goes on without the headset";
}

void OpenXR::pollEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			stop();
			return;
		}
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const XrSessionState state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
			qInfo() << "OpenXR: session" << stateName(state);
			if (state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				setRunning(ok(xrBeginSession(session, &beginInfo), "xrBeginSession"));
				recenterPending = true;
				// Once: the headset put back on comes here again.
				if (running && !setUp)
				{
					setUp = true;
					StelCore* core = StelApp::getInstance().getCore();
					core->setCurrentProjectionType(StelCore::ProjectionPerspective);
					core->getMovementMgr()->setMountMode(StelMovementMgr::MountAltAzimuthal);
					core->getMovementMgr()->setFlagTracking(false);
					panelPlacePending = true;
					menu.setHome(core->getCurrentLocation());
					// Thousands of them cost about 6 ms a frame on the Frame; the
					// menu's Satellites tile brings them back.
					// And the atmosphere, worked out for every view, costs the most by day.
					for (const char* costly : {"actionShow_Satellite_Hints", "actionShow_Atmosphere"})
						if (StelAction* action = StelApp::getInstance().getStelActionManager()->findAction(costly))
							action->setChecked(false);
					// Going to another world, Stellarium sets the atmosphere as
					// saved, so saved off, as here; and the ground as that world's
					// own, for the session only: saved, it would also give a start
					// located by IP a flat ground instead of the landscape chosen.
					StelApp::getInstance().getSettings()->setValue("landscape/flag_atmosphere", false);
					GETSTELMODULE(LandscapeMgr)->setFlagEnvironmentAutoEnable(true);
					GETSTELMODULE(LandscapeMgr)->setFlagLandscapeAutoSelection(true);
					// The toolbars show on hover at the window's edges, which a
					// pointer on a panel hardly finds.
					if (QObject* gui = dynamic_cast<QObject*>(StelApp::getInstance().getGui()))
					{
						gui->setProperty("autoHideHorizontalButtonBar", false);
						gui->setProperty("autoHideVerticalButtonBar", false);
					}
					// They still wait for the mouse to come by once.
					const QSize size = StelMainView::getInstance().viewport()->size();
					postMouse(QEvent::MouseMove, QPointF(1., size.height() - 1.), Qt::NoButton, Qt::NoButton);
					postMouse(QEvent::MouseMove, QPointF(1., size.height() / 2.), Qt::NoButton, Qt::NoButton);
					panelTimer.start();
				}
			}
			else if (state == XR_SESSION_STATE_STOPPING)
			{
				setRunning(false);
				ok(xrEndSession(session), "xrEndSession");
			}
			else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
			{
				stop();
				return;
			}
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

void OpenXR::drawFrame()
{
	StelApp& app = StelApp::getInstance();
	if (!failed && !session && !start())
		stop();
	if (instance)
		pollEvents();
	if (!running)
	{
		app.draw();
		if (session)
			StelMainView::getInstance().thereWasAnEvent(); // poll the session often until it runs
		return;
	}

	if (!clock.isValid())
		clock.start();
	// Everything else Stellarium does per frame: updating, the GUI, events.
	if (lastFrameEnd)
	{
		timing.outside += clock.nsecsElapsed() - lastFrameEnd;
		const QHash<QString, qint64>& updates = app.getUpdateTimes();
		for (auto it = updates.begin(); it != updates.end(); ++it)
			timing.updates[it.key()] += it.value();
	}

	XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frameState{XR_TYPE_FRAME_STATE};
	XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
	const qint64 waitStart = clock.nsecsElapsed();
	const bool waited = ok(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame");
	timing.waiting += clock.nsecsElapsed() - waitStart;
	if (!waited || !ok(xrBeginFrame(session, &beginInfo), "xrBeginFrame"))
	{
		stop(); // a runtime gone sends no events, only errors
		app.draw();
		return;
	}

	handleInput(frameState.predictedDisplayTime);
	const auto refresh = [this](Quad& quad, const QImage& image) {
		if (!image.isNull() && (quad.swapchain || createQuad(quad, image.width(), image.height(), false)))
			uploadQuad(quad, image);
	};
	// The menu and the card work out their text (the card's rise and set times
	// among it) to see whether anything changed: often enough to follow the
	// clock, and at once when the pointer moves to another tile.
	if (handHover != drawnHandHover || cardHover != drawnCardHover || !menuClock.isValid() || menuClock.elapsed() >= 250)
	{
		drawnHandHover = handHover;
		drawnCardHover = cardHover;
		menuClock.start();
		if (handMenuShown || !handMenuQuad.swapchain)
			refresh(handMenuQuad, menu.render(OpenXRMenu::Hand, handHover));
		if (OpenXRMenu::hasCard())
			refresh(cardQuad, menu.render(OpenXRMenu::Card, cardHover));
	}
	watchTime();
	if (timeBarShown)
		refresh(timeBarQuad, menu.renderTimeBar());
	if (panelImageNew)
	{
		if (panel.width != panelImage.width() || panel.height != panelImage.height())
		{
			if (createQuad(panel, panelImage.width(), panelImage.height(), false))
				qInfo().nospace() << "OpenXR: GUI panel " << panel.width << "x" << panel.height << " pixels, "
						  << qRound(2. * std::atan(0.5 * panelWidth() / panelDistance) * M_180_PI) << "° wide";
			else
				panelImage = QImage();
		}
		if (!panelImage.isNull())
			uploadQuad(panel, panelImage);
		panelImageNew = false;
	}

	XrCompositionLayerProjectionView layerViews[2] = {};
	XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.space = localSpace;
	layer.viewCount = 2;
	layer.views = layerViews;
	const bool drawn = frameState.shouldRender && drawEyes(frameState.predictedDisplayTime, layerViews);

	// The GUI and the cursor float over the sky, blended by their alpha
	// (premultiplied, as QPainter leaves it).
	XrCompositionLayerQuad quads[10];
	for (XrCompositionLayerQuad& q : quads)
		q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	const XrCompositionLayerBaseHeader* layers[11] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
	uint32_t layerCount = 1;
	// Sized by width, the height following the image, unless given.
	auto addQuad = [&](XrSwapchain swapchain, int width, int height, const XrPosef& pose, float meters, float metersHigh = 0.f,
			   XrSpace space = XR_NULL_HANDLE) {
		XrCompositionLayerQuad& q = quads[layerCount - 1];
		q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		q.space = space ? space : localSpace;
		q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		q.subImage.swapchain = swapchain;
		q.subImage.imageRect.extent = {width, height};
		q.pose = pose;
		q.size = {meters, metersHigh > 0.f ? metersHigh : meters * height / width};
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
	};
	// Farthest first, as the runtime stacks them in this order: the card out
	// in the sky, the GUI, the time bar and arrow a meter or so off, then
	// what is at hand.
	if (cardShown)
		addQuad(cardQuad.swapchain, cardQuad.width, cardQuad.height, cardPose, cardMeters());
	if (loupeDrawn)
	{
		addQuad(loupe.swapchain, loupe.width, loupe.height, loupePose, loupeMeters);
		addQuad(loupeRim.swapchain, loupeRim.width, loupeRim.height, loupePose, loupeMeters);
	}
	if (panelVisible && panel.swapchain)
		addQuad(panel.swapchain, panel.width, panel.height, panelPose, panelWidth());
	if (timeBarShown && timeBarQuad.swapchain)
		addQuad(timeBarQuad.swapchain, timeBarQuad.width, timeBarQuad.height, {{0.f, 0.f, 0.f, 1.f}, timeBarPlace}, timeBarWidth,
			0.f, viewSpace);
	if (arrowVisible)
		addQuad(arrowQuad.swapchain, arrowQuad.width, arrowQuad.height, arrowPose, arrowSize);
	if (handMenuShown)
		addQuad(handMenuQuad.swapchain, handMenuQuad.width, handMenuQuad.height, handMenuPose, handMenuMeters());
	if (cursorVisible)
		addQuad(laserQuad.swapchain, laserQuad.width, laserQuad.height, laserPose, laserLength, laserThickness);
	if (cursorVisible)
	{
		const Quad& shown = cursorInSky ? skyCursor : cursor;
		addQuad(shown.swapchain, shown.width, shown.height, cursorPose, cursorSize);
	}

	XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = drawn ? layerCount : 0;
	endInfo.layers = layers;
	ok(xrEndFrame(session, &endInfo), "xrEndFrame");

	if (!drawn)
		app.draw(); // the headset shows nothing, so the window draws on its own
	else
	{
		++framesDrawn;
		if (!rateTimer.isValid())
			rateTimer.start();
		else if (rateTimer.elapsed() >= 10000)
		{
			qInfo().nospace() << "OpenXR: " << qRound(framesDrawn * 1000. / rateTimer.restart()) << " frames per second";
			// Per frame, in ms. Waiting is xrWaitFrame's pacing; outside is
			// Stellarium's own update and the GUI; the CPU draws include the
			// driver's work submitting them. The GPU time covers all views.
			const auto ms = [this](qint64 ns) { return QString::number(ns / 1e6 / framesDrawn, 'f', 1); };
			qInfo().noquote() << "OpenXR: ms per frame: waiting" << ms(timing.waiting) << "outside" << ms(timing.outside)
					  << "atmosphere" << ms(timing.atmosphere) << "sky draws (CPU)" << ms(timing.draws)
					  << "views (GPU)" << (timing.gpuFrames ? QString::number(timing.gpu / 1e6 / timing.gpuFrames, 'f', 1) : QString("n/a"));
			// The slowest updates, and the rest of outside: the GUI and events.
			QList<QPair<qint64, QString>> slowest;
			qint64 updating = 0;
			for (auto it = timing.updates.begin(); it != timing.updates.end(); ++it)
			{
				slowest.append({it.value(), it.key()});
				updating += it.value();
			}
			std::sort(slowest.rbegin(), slowest.rend());
			QStringList top;
			for (int i = 0; i < qMin(5, int(slowest.size())); ++i)
				top << slowest[i].second + " " + ms(slowest[i].first);
			qInfo().noquote() << "OpenXR: ms per frame in updates:" << top.join(", ") << "| GUI and events" << ms(timing.outside - updating);
			timing = Timing();
			framesDrawn = 0;
		}
	}
	lastFrameEnd = clock.nsecsElapsed();
}

bool OpenXR::drawEyes(XrTime time, XrCompositionLayerProjectionView* layerViews)
{
	XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = localSpace;
	XrViewState viewState{XR_TYPE_VIEW_STATE};
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t viewCount = 0;
	XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
	if (!ok(xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views), "xrLocateViews")
	    || !ok(xrLocateSpace(viewSpace, localSpace, time, &head), "xrLocateSpace")
	    || !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
	    || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return false;

	StelApp& app = StelApp::getInstance();
	StelCore* core = app.getCore();
	StelMovementMgr* mmgr = core->getMovementMgr();
	const XrQuaternionf& q = head.pose.orientation;
	if (recenterPending)
	{
		const Vec3d wanted = core->j2000ToAltAz(mmgr->getViewDirectionJ2000(), StelCore::RefractionOff);
		const Vec3d ahead = toAltAz(q, {0, 0, -1}, 0.);
		yaw = std::atan2(wanted[1], wanted[0]) - std::atan2(ahead[1], ahead[0]);
		recenterPending = false;
	}
	if (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	{
		headPose = head.pose;
		if (panelPlacePending)
			placePanel(head.pose);
	}
	placeArrow(head.pose);
	// The window, object picking and the magnitude limits follow the head from
	// the next update on, the limits by the eyes' field, which seldom changes.
	mmgr->setViewDirectionJ2000(core->altAzToJ2000(toAltAz(q, {0, 0, -1}, yaw), StelCore::RefractionOff));
	mmgr->setViewUpVectorJ2000(core->altAzToJ2000(toAltAz(q, {0, 1, 0}, yaw), StelCore::RefractionOff));
	const double eyeFov = verticalFov(views[0].fov);
	if (std::fabs(mmgr->getAimFov() - eyeFov) > 0.01)
		mmgr->zoomTo(eyeFov, 0.f);

	QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
	GLint windowFbo = 0, viewport[4] = {};
	gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &windowFbo);
	gl->glGetIntegerv(GL_VIEWPORT, viewport);
	// Qt's painting of the window can leave a scissor box of the window's size,
	// which would clip the first eye's sky outside it.
	const bool scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
	gl->glDisable(GL_SCISSOR_TEST);
	const StelProjector::StelProjectorParams baseParams = core->getCurrentStelProjectorParams();

	// GPU time of all views, read a few frames later so as not to wait for it.
	constexpr GLenum timeElapsed = 0x88BF, queryResult = 0x8866, queryResultAvailable = 0x8867;
	if (!gpuQueries[0])
		gl->glGenQueries(3, gpuQueries);
	const GLuint query = gpuQueries[gpuQuery];
	if (gpuQueryPending[gpuQuery])
	{
		GLuint available = 0, ns = 0;
		gl->glGetQueryObjectuiv(query, queryResultAvailable, &available);
		if (available)
		{
			gl->glGetQueryObjectuiv(query, queryResult, &ns);
			timing.gpu += ns;
			++timing.gpuFrames;
		}
	}
	gl->glBeginQuery(timeElapsed, query);
	// The loupe first: the first view of a frame sets the eye's adaptation for
	// the others, and its projection is not to stay for the next update.
	loupeDrawn = loupeShown && drawLoupe(baseParams);

	// Shared: everything in the sky is at infinity, so the eyes see the same
	// image but for its direction and extent. One view from the head covering
	// both goes to both eyes, and the runtime reprojects it for each.
	if (sharedView && !sky.swapchain && !createSharedView(views))
		sharedView = false;
	Eye* targets[2] = {sharedView ? &sky : &eyes[0], &eyes[1]};
	XrPosef poses[2] = {views[0].pose, views[1].pose};
	XrFovf fovs[2] = {views[0].fov, views[1].fov};
	if (sharedView)
	{
		const XrVector3f& a = views[0].pose.position, &b = views[1].pose.position;
		poses[0] = poses[1] = {head.pose.orientation, {(a.x + b.x) / 2.f, (a.y + b.y) / 2.f, (a.z + b.z) / 2.f}};
		fovs[0] = fovs[1] = skyFov;
	}

	// All views are drawn before waiting for any, so the GPU works on one
	// while the CPU sends the next; then all go to the runtime together.
	const int viewCountToDraw = sharedView ? 1 : 2;
	bool drawn = true, acquired[2] = {false, false};
	for (int i = 0; i < viewCountToDraw && drawn; ++i)
	{
		Eye& e = *targets[i];
		uint32_t index = 0;
		XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		imageWait.timeout = XR_INFINITE_DURATION;
		acquired[i] = ok(xrAcquireSwapchainImage(e.swapchain, nullptr, &index), "xrAcquireSwapchainImage");
		drawn = acquired[i] && ok(xrWaitSwapchainImage(e.swapchain, &imageWait), "xrWaitSwapchainImage");
		if (drawn)
			drawView(e, index, fovs[i], poses[i].orientation, baseParams, i == 0 && !loupeDrawn);
	}
	for (int i = 0; i < 2; ++i)
	{
		const Eye& e = *targets[sharedView ? 0 : i];
		layerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		layerViews[i].pose = poses[i];
		layerViews[i].fov = fovs[i];
		layerViews[i].subImage.swapchain = e.swapchain;
		layerViews[i].subImage.imageRect.extent = {e.width, e.height};
	}

	finishDrawing();
	for (int i = 0; i < 2; ++i)
		if (acquired[i])
			ok(xrReleaseSwapchainImage(targets[i]->swapchain, nullptr), "xrReleaseSwapchainImage");
	if (loupeDrawn)
		ok(xrReleaseSwapchainImage(loupe.swapchain, nullptr), "xrReleaseSwapchainImage(loupe)");
	gl->glEndQuery(timeElapsed);
	gpuQueryPending[gpuQuery] = true;
	gpuQuery = (gpuQuery + 1) % 3;

	// The view's projection stays for the next update, so the atmosphere works
	// out its grid over the same viewport and needn't build it again; the
	// window's comes back with the session's end.
	gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(windowFbo));
	gl->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (scissor)
		gl->glEnable(GL_SCISSOR_TEST);
	return drawn;
}

bool OpenXR::createActions()
{
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	qstrncpy(setInfo.actionSetName, "stellarium", XR_MAX_ACTION_SET_NAME_SIZE);
	qstrncpy(setInfo.localizedActionSetName, "Stellarium", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
	if (!ok(xrCreateActionSet(instance, &setInfo, &actionSet), "xrCreateActionSet"))
		return false;

	xrStringToPath(instance, "/user/hand/left", &handPaths[0]);
	xrStringToPath(instance, "/user/hand/right", &handPaths[1]);
	auto create = [&](XrAction* action, XrActionType type, const char* name, const char* localized) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		qstrncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE);
		qstrncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
		info.countSubactionPaths = 2;
		info.subactionPaths = handPaths;
		return ok(xrCreateAction(actionSet, &info, action), name);
	};
	if (!create(&aimAction, XR_ACTION_TYPE_POSE_INPUT, "aim", "Point")
	    || !create(&triggerAction, XR_ACTION_TYPE_FLOAT_INPUT, "click", "Click")
	    || !create(&stickAction, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Scrub time and zoom the loupe; go through the menu")
	    || !create(&hapticAction, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Feedback"))
		return false;
	const char* const buttonNames[Buttons][2] = {
		{"pause", "Pause"}, {"deselect", "Deselect"}, {"now", "Back to now"}, {"zoom_to_object", "Loupe on the object"},
		{"menu", "Show or hide the menu"}, {"gui", "Show or hide the full GUI"},
		{"faster", "Faster"}, {"slower", "Slower"}, {"hour_back", "An hour back"}, {"hour_on", "An hour on"},
		{"bumper", "Constellation lines; ground"}, {"grip_button", "Constellation art; atmosphere"},
		{"stick_click", "Recenter"}};
	for (int b = 0; b < Buttons; ++b)
		if (!create(&buttonActions[b], XR_ACTION_TYPE_BOOLEAN_INPUT, buttonNames[b][0], buttonNames[b][1]))
			return false;

	using Bindings = std::vector<std::pair<XrAction, const char*>>;
	const Bindings common = {
		{aimAction, "left/input/aim/pose"}, {aimAction, "right/input/aim/pose"},
		{hapticAction, "left/output/haptic"}, {hapticAction, "right/output/haptic"}};
	const auto with = [&common](Bindings more) {
		more.insert(more.end(), common.begin(), common.end());
		return more;
	};
	// SteamVR on the Frame remaps Touch bindings when the Frame's own profile
	// is missing.
	bool any = false;
	if (frameControllerExtension)
		any |= suggestBindings("/interaction_profiles/valve/frame_controller_valve", with({
			{triggerAction, "left/input/trigger/value"}, {triggerAction, "right/input/trigger/value"},
			{stickAction, "left/input/thumbstick"}, {stickAction, "right/input/thumbstick"},
			{buttonActions[A], "right/input/a/click"}, {buttonActions[B], "right/input/b/click"},
			{buttonActions[X], "right/input/x/click"}, {buttonActions[Y], "right/input/y/click"},
			{buttonActions[Menu], "right/input/menu/click"}, {buttonActions[View], "left/input/view/click"},
			{buttonActions[Up], "left/input/dpad_up/click"}, {buttonActions[Down], "left/input/dpad_down/click"},
			{buttonActions[Left], "left/input/dpad_left/click"}, {buttonActions[Right], "left/input/dpad_right/click"},
			{buttonActions[Bumper], "left/input/shoulder/click"}, {buttonActions[Bumper], "right/input/shoulder/click"},
			{buttonActions[Grip], "left/input/squeeze/click"}, {buttonActions[Grip], "right/input/squeeze/click"},
			{buttonActions[Stick], "left/input/thumbstick/click"}, {buttonActions[Stick], "right/input/thumbstick/click"}}));
	any |= suggestBindings("/interaction_profiles/oculus/touch_controller", with({
		{triggerAction, "left/input/trigger/value"}, {triggerAction, "right/input/trigger/value"},
		{stickAction, "left/input/thumbstick"}, {stickAction, "right/input/thumbstick"},
		{buttonActions[A], "right/input/a/click"}, {buttonActions[B], "right/input/b/click"},
		{buttonActions[X], "left/input/x/click"}, {buttonActions[Y], "left/input/y/click"},
		{buttonActions[Menu], "left/input/menu/click"},
		{buttonActions[Grip], "left/input/squeeze/value"}, {buttonActions[Grip], "right/input/squeeze/value"},
		{buttonActions[Stick], "left/input/thumbstick/click"}, {buttonActions[Stick], "right/input/thumbstick/click"}}));
	// Index controllers: their system buttons stay SteamVR's, so the trackpads
	// stand in for the menu and view buttons.
	any |= suggestBindings("/interaction_profiles/valve/index_controller", with({
		{triggerAction, "left/input/trigger/value"}, {triggerAction, "right/input/trigger/value"},
		{stickAction, "left/input/thumbstick"}, {stickAction, "right/input/thumbstick"},
		{buttonActions[A], "right/input/a/click"}, {buttonActions[B], "right/input/b/click"},
		{buttonActions[X], "left/input/a/click"}, {buttonActions[Y], "left/input/b/click"},
		{buttonActions[Menu], "right/input/trackpad/force"}, {buttonActions[View], "left/input/trackpad/force"},
		{buttonActions[Grip], "left/input/squeeze/value"}, {buttonActions[Grip], "right/input/squeeze/value"},
		{buttonActions[Stick], "left/input/thumbstick/click"}, {buttonActions[Stick], "right/input/thumbstick/click"}}));
	any |= suggestBindings("/interaction_profiles/khr/simple_controller", with({
		{triggerAction, "left/input/select/click"}, {triggerAction, "right/input/select/click"},
		{buttonActions[Menu], "left/input/menu/click"}, {buttonActions[Menu], "right/input/menu/click"}}));
	// Windows Mixed Reality, which Monado's keyboard-driven test controllers are.
	any |= suggestBindings("/interaction_profiles/microsoft/motion_controller", with({
		{triggerAction, "left/input/trigger/value"}, {triggerAction, "right/input/trigger/value"},
		{stickAction, "left/input/thumbstick"}, {stickAction, "right/input/thumbstick"},
		{buttonActions[Menu], "right/input/menu/click"}, {buttonActions[View], "left/input/menu/click"},
		{buttonActions[A], "right/input/trackpad/click"}, {buttonActions[Y], "left/input/trackpad/click"},
		{buttonActions[Grip], "left/input/squeeze/click"}, {buttonActions[Grip], "right/input/squeeze/click"},
		{buttonActions[Stick], "left/input/thumbstick/click"}, {buttonActions[Stick], "right/input/thumbstick/click"}}));
	if (!any)
		return false;

	XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &actionSet;
	if (!ok(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets"))
		return false;

	for (int h = 0; h < 2; ++h)
	{
		XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		spaceInfo.action = aimAction;
		spaceInfo.subactionPath = handPaths[h];
		spaceInfo.poseInActionSpace.orientation.w = 1.f;
		if (!ok(xrCreateActionSpace(session, &spaceInfo, &aimSpaces[h]), "xrCreateActionSpace"))
			return false;
	}
	return true;
}

bool OpenXR::suggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings)
{
	std::vector<XrActionSuggestedBinding> suggested;
	for (const auto& [action, input] : bindings)
	{
		XrPath path = XR_NULL_PATH;
		xrStringToPath(instance, QByteArray("/user/hand/").append(input).constData(), &path);
		suggested.push_back({action, path});
	}
	XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	xrStringToPath(instance, profile, &info.interactionProfile);
	info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
	info.suggestedBindings = suggested.data();
	return ok(xrSuggestInteractionProfileBindings(instance, &info), profile);
}

void OpenXR::handleInput(XrTime time)
{
	cursorVisible = false;
	const XrActiveActionSet active{actionSet, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	if (xrSyncActions(session, &syncInfo) != XR_SUCCESS)
		return; // not focused, e.g. under SteamVR's menu

	// A trigger pressed on the other hand makes that one the pointer.
	bool pressed = false, released = false;
	for (int h = 0; h < 2; ++h)
	{
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = triggerAction;
		info.subactionPath = handPaths[h];
		XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
		xrGetActionStateFloat(session, &info, &trigger);
		if (!triggerDown[h] && trigger.currentState > triggerPress)
		{
			triggerDown[h] = true;
			if (h == activeHand || !triggerDown[activeHand])
			{
				activeHand = h;
				pressed = true;
			}
		}
		else if (triggerDown[h] && trigger.currentState < triggerRelease)
		{
			triggerDown[h] = false;
			released |= h == activeHand;
		}
		for (int b = 0; b < Buttons; ++b)
		{
			info.action = buttonActions[b];
			XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
			xrGetActionStateBoolean(session, &info, &state);
			const bool wasDown = buttonDown[b][h];
			buttonDown[b][h] = state.isActive && state.currentState;
			if (buttonDown[b][h] && !wasDown)
				press(b, h);
		}
	}
	const auto stickOf = [this](int hand) {
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = stickAction;
		info.subactionPath = handPaths[hand];
		XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
		xrGetActionStateVector2f(session, &info, &stick);
		return stick.isActive ? QPointF(stick.currentState.x, stick.currentState.y) : QPointF();
	};
	const QPointF rightStick = stickOf(rightHand), leftStick = stickOf(leftHand);
	const float stickX = deadZone(float(rightStick.x()));
	const float stickY = deadZone(float(rightStick.y()));
	// The left stick, flicked, goes to the next category or page of the menu.
	const QPointF flick = leftStick;
	if (std::fabs(flick.x()) < flickOff && std::fabs(flick.y()) < flickOff)
		flickArmed = true;
	else if (flickArmed && handMenuShown && (std::fabs(flick.x()) > flickOn || std::fabs(flick.y()) > flickOn))
	{
		flickArmed = false;
		pulse(0.15f, 10);
		const bool across = std::fabs(flick.x()) > std::fabs(flick.y());
		// Up goes back a page, as it would scroll a list.
		const int by = across ? (flick.x() > 0. ? 1 : -1) : (flick.y() > 0. ? -1 : 1);
		QMetaObject::invokeMethod(this, [this, across, by] {
			if (across)
				menu.turnCategory(by);
			else
				menu.turnPage(by);
			menu.changed();
			menuClock.invalidate();
		}, Qt::QueuedConnection);
	}

	const double dt = lastInputTime ? qBound(0., (time - lastInputTime) * 1e-9, 0.1) : 0.;
	lastInputTime = time;
	placeHandMenu(time);
	// Let go of the GUI before anything that needs the pointer: a controller
	// asleep with the trigger down would otherwise leave it dragging.
	if (released && guiPressed)
	{
		postMouse(QEvent::MouseButtonRelease, lastMouse, Qt::LeftButton, Qt::NoButton);
		guiPressed = false;
	}
	XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	const auto tracked = [&](int hand) {
		return XR_SUCCEEDED(xrLocateSpace(aimSpaces[hand], localSpace, time, &aim)) && (aim.locationFlags & valid) == valid;
	};
	aimValid = tracked(activeHand);
	// One controller only, or the other one asleep: point with the one there is.
	if (!aimValid && !triggerDown[activeHand] && tracked(1 - activeHand))
	{
		activeHand = 1 - activeHand;
		aimValid = true;
	}
	if (!aimValid)
		return;
	const QVector3D origin = toQt(aim.pose.position);
	const QVector3D dir = toQt(aim.pose.orientation).rotatedVector(QVector3D(0.f, 0.f, -1.f));
	placeCard();

	QPointF uv;
	float distance = 0.f;
	const QSize handSize = OpenXRMenu::size(OpenXRMenu::Hand), cardSize = OpenXRMenu::size(OpenXRMenu::Card);
	const bool onHand = handMenuShown && hitQuad(handMenuPose, handMenuMeters(), handSize, origin, dir, &uv, &distance);
	handHover = onHand ? menu.tileAt(OpenXRMenu::Hand, QPointF(uv.x() * handSize.width(), uv.y() * handSize.height())) : -1;
	const bool onCard = !onHand && cardShown && hitQuad(cardPose, cardMeters(), cardSize, origin, dir, &uv, &distance);
	cardHover = onCard ? menu.tileAt(OpenXRMenu::Card, QPointF(uv.x() * cardSize.width(), uv.y() * cardSize.height())) : -1;
	const bool onMenu = onHand || onCard;
	const bool onPanel = !onMenu && panelVisible && panel.swapchain
			     && hitQuad(panelPose, panelWidth(), panelImage.size(), origin, dir, &uv, &distance);
	const QPointF pixel(uv.x() * panelImage.width() / panelScale, uv.y() * panelImage.height() / panelScale);
	// Transparent parts of the panel let the pointer through to the sky.
	const QPoint p = (pixel * panelScale).toPoint();
	const bool onGui = onPanel && panelImage.valid(p) && qAlpha(panelImage.pixel(p)) > 0;
	const QGraphicsItem* guiTarget = onGui ? clickable(pixel) : nullptr;
	// Its round view only; the uv kept is where in it.
	QPointF loupeUv;
	float loupeDistance = 0.f;
	const bool onLoupe = !onMenu && !onGui && loupeShown
			     && hitQuad(loupePose, loupeMeters, QSize(loupePixels, loupePixels), origin, dir, &loupeUv, &loupeDistance)
			     && QVector2D(loupeUv - QPointF(0.5, 0.5)).length() < 0.5f * eyepieceRadius;

	// A light tick on reaching something a click would hit.
	const quintptr target = onHand && handHover >= 0   ? quintptr(handHover + 1)
				: onCard && cardHover >= 0 ? quintptr(cardHover + 1 + 1000)
							   : quintptr(guiTarget);
	if (target && target != lastTarget)
		pulse(0.15f, 10);
	lastTarget = target;

	cursorVisible = true;
	if (onMenu)
	{
		// Just in front, about 1.3° across on the hand, 0.6° on the card.
		cursorPose.position = toXr(origin + (distance - 0.005f) * dir);
		cursorPose.orientation = onHand ? handMenuPose.orientation : cardPose.orientation;
		cursorSize = onHand ? 0.01f : 0.017f * distance;
	}
	else if (onPanel)
	{
		cursorPose.position = toXr(origin + (distance - 0.005f) * dir);
		cursorPose.orientation = panelPose.orientation;
		cursorSize = 0.02f;
		if (pixel != lastMouse)
		{
			postMouse(QEvent::MouseMove, pixel, Qt::NoButton, guiPressed ? Qt::LeftButton : Qt::NoButton);
			panelInput.start();
		}
		lastMouse = pixel;
	}
	else
	{
		// The sky is infinitely far, so a pick goes by the ray's direction
		// alone, wherever the hand is. Put out from the head along it, the dot
		// sits right over what it picks; far out, the two eyes see it nearly
		// where they see the stars. The beam, parallel, runs towards it. On the
		// loupe, the pick goes by where the ray meets it, and the dot sits there.
		constexpr float skyDistance = 50.f;
		const QVector3D at = onLoupe ? origin + (loupeDistance - 0.01f) * dir : toQt(headPose.position) + skyDistance * dir.normalized();
		cursorPose = {onLoupe ? loupePose.orientation : toXr(facingHead(at, headPose)), toXr(at)};
		const double halfDegrees = 0.5 * skyDotDegrees * (skyCursorPixels / 2.) / skyDotRadius;
		cursorSize = static_cast<float>(2. * (at - toQt(headPose.position)).length() * std::tan(halfDegrees / M_180_PI));
	}
	cursorInSky = !onMenu && !onPanel;

	// The beam runs to what it points at, or fades out into the sky.
	laserLength = onMenu || onPanel ? distance : 2.f;
	{
		const QVector3D along = dir.normalized();
		QVector3D facing = toQt(headPose.position) - (origin + 0.5f * laserLength * along);
		facing -= QVector3D::dotProduct(facing, along) * along;
		if (facing.lengthSquared() < 1e-6f)
			facing = QVector3D(0.f, 1.f, 0.f);
		facing.normalize();
		laserPose.orientation = toXr(QQuaternion::fromAxes(along, QVector3D::crossProduct(facing, along), facing));
		laserPose.position = toXr(origin + 0.5f * laserLength * along);
	}

	if (pressed)
		qDebug() << "OpenXR: trigger" << (onMenu ? "on the menu" : onGui ? "on the GUI" : "in the sky");
	if (pressed && onMenu)
	{
		// Not from inside the window's paint, which this runs in.
		const int tile = onHand ? handHover : cardHover;
		if (tile >= 0)
		{
			pulse(0.5f, 20);
			QMetaObject::invokeMethod(this, [this, act = menu.actionFor(onHand ? OpenXRMenu::Hand : OpenXRMenu::Card, tile)] {
				if (act)
					act();
				menu.changed();
				menuClock.invalidate();
			}, Qt::QueuedConnection);
		}
	}
	else if (pressed && onGui)
	{
		if (guiTarget)
		{
			pulse(0.5f, 20);
			postMouse(QEvent::MouseButtonPress, pixel, Qt::LeftButton, Qt::LeftButton);
			guiPressed = true;
		}
	}
	else if (pressed && onLoupe)
	{
		// What shows under the dot in the loupe's view, at its magnification.
		// Another object picked there is the loupe's to show instead; nothing
		// there leaves it as it is.
		pulse(0.3f, 15);
		StelCore* core = StelApp::getInstance().getCore();
		const float across = static_cast<float>(std::tan(0.5 * loupeFov / M_180_PI));
		const QVector3D view((2.f * float(loupeUv.x()) - 1.f) * across, (1.f - 2.f * float(loupeUv.y())) * across, -1.f);
		const Vec3d j2000 = core->altAzToJ2000(toAltAz({0.f, 0.f, 0.f, 1.f}, loupeTurn.rotatedVector(view), yaw), StelCore::RefractionAuto);
		QMetaObject::invokeMethod(this, [this, j2000, magnification = loupeDegrees / loupeFov, limit = loupeLimit] {
			const StelObjectP object = pickAlong(StelApp::getInstance().getCore(), j2000, magnification, limit);
			if (object)
			{
				loupeObject = object;
				GETSTELMODULE(StelObjectMgr)->setSelectedObject(object);
			}
			qDebug() << "OpenXR: picked in the loupe" << (object ? object->getEnglishName() : QString("nothing"));
		}, Qt::QueuedConnection);
	}
	else if (pressed)
	{
		// What shows under the dot.
		pulse(0.3f, 15);
		StelCore* core = StelApp::getInstance().getCore();
		const Vec3d j2000 = core->altAzToJ2000(toAltAz({0.f, 0.f, 0.f, 1.f}, dir, yaw), StelCore::RefractionAuto);
		QMetaObject::invokeMethod(this, [j2000] {
			StelCore* core = StelApp::getInstance().getCore();
			const StelObjectP object = pickAlong(core, j2000, 1., core->getSkyDrawer()->getLimitMagnitude());
			GETSTELMODULE(StelObjectMgr)->setSelectedObject(object);
			qDebug() << "OpenXR: picked" << (object ? object->getEnglishName() : QString("nothing"));
		}, Qt::QueuedConnection);
	}
	if (onGui && stickY != 0.f)
	{
		const QPoint angle(0, qRound(stickY * 20.f)); // about 12 notches a second at full tilt
		QWidget* viewport = StelMainView::getInstance().viewport();
		QCoreApplication::postEvent(viewport, new QWheelEvent(pixel, viewport->mapToGlobal(pixel), QPoint(), angle,
								      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false));
	}
	else if (!onMenu && !onPanel)
	{
		// Squared, for fine steps near the middle.
		if (stickX != 0.f)
		{
			StelCore* core = StelApp::getInstance().getCore();
			core->setJD(core->getJD() + double(stickX) * std::fabs(stickX) * scrubHoursPerSecond / 24. * dt);
		}
		// Up narrows the loupe, zooming in.
		if (stickY != 0.f && loupeFov > 0.)
			loupeFov = qBound(minLoupeFov, loupeFov * std::exp(-stickY * zoomPerSecond * dt), maxLoupeFov);
	}
}

void OpenXR::press(int button, int hand)
{
	pulse(0.3f, 15);
	if (button == Menu)
	{
		handMenuVisible = !handMenuVisible;
		return;
	}
	if (button == View)
	{
		panelVisible = !panelVisible;
		panelPlacePending = panelVisible;
		if (panelVisible)
			panelTimer.start();
		return;
	}
	if (button == Stick)
	{
		recenter();
		return;
	}
	if (button == Y)
	{
		// The loupe closed if open, else opened on the object picked.
		const QList<StelObjectP> selected = GETSTELMODULE(StelObjectMgr)->getSelectedObject();
		if (loupeFov > 0.)
			loupeFov = 0.;
		else if (!selected.isEmpty())
		{
			loupeObject = selected.first();
			const double radius = loupeObject->getAngularRadius(StelApp::getInstance().getCore());
			loupeFov = radius > 0. ? qBound(minLoupeFov, loupeFovPerRadius * radius, maxLoupeFov) : starLoupeFov;
		}
		return;
	}

	// The rest run Stellarium's actions, outside the window's paint.
	const char* action = nullptr;
	switch (button)
	{
		case A: action = StelApp::getInstance().getCore()->getTimeRate() == 0. ? "actionSet_Real_Time_Speed" : "actionSet_Time_Rate_Zero"; break;
		case X: action = "actionReturn_To_Current_Time"; break;
		case Up: action = "actionIncrease_Time_Speed"; break;
		case Down: action = "actionDecrease_Time_Speed"; break;
		case Left: action = "actionSubtract_Solar_Hour"; break;
		case Right: action = "actionAdd_Solar_Hour"; break;
		case Bumper: action = hand == rightHand ? "actionShow_Constellation_Lines" : "actionShow_Ground"; break;
		case Grip: action = hand == rightHand ? "actionShow_Constellation_Art" : "actionShow_Atmosphere"; break;
		default: break;
	}
	QMetaObject::invokeMethod(this, [this, button, action] {
		if (button == B)
			GETSTELMODULE(StelObjectMgr)->unSelect();
		else if (StelAction* a = action ? StelApp::getInstance().getStelActionManager()->findAction(action) : nullptr)
		{
			if (a->isCheckable())
				a->setChecked(!a->isChecked());
			else
				a->trigger();
		}
		menu.changed();
		menuClock.invalidate();
	}, Qt::QueuedConnection);
}

float OpenXR::panelWidth() const
{
	const double logicalWidth = panelImage.width() / panelScale;
	const double degrees = qMin(maxPanelDegrees, logicalWidth / StelApp::getInstance().getGuiFontSize() * textDegrees);
	return static_cast<float>(2. * panelDistance * std::tan(0.5 * degrees / M_180_PI));
}

bool OpenXR::hitQuad(const XrPosef& pose, float width, const QSize& size, const QVector3D& origin,
		     const QVector3D& dir, QPointF* uv, float* distance)
{
	if (size.isEmpty())
		return false;
	// The quad lies in its pose's XY plane and faces +Z.
	const QQuaternion rotation = toQt(pose.orientation);
	const QVector3D center = toQt(pose.position);
	const QVector3D normal = rotation.rotatedVector(QVector3D(0.f, 0.f, 1.f));
	const float along = QVector3D::dotProduct(dir, normal);
	if (along > -1e-4f)
		return false; // from behind or edge-on
	const float t = QVector3D::dotProduct(center - origin, normal) / along;
	if (t <= 0.f)
		return false;
	const QVector3D local = rotation.conjugated().rotatedVector(origin + t * dir - center);
	const float height = width * size.height() / size.width();
	const float u = local.x() / width + 0.5f, v = 0.5f - local.y() / height;
	if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
		return false;
	*uv = QPointF(u, v);
	*distance = t;
	return true;
}

void OpenXR::placePanel(const XrPosef& head)
{
	QVector3D ahead = toQt(head.orientation).rotatedVector(QVector3D(0.f, 0.f, -1.f));
	ahead.setY(0.f);
	if (ahead.lengthSquared() < 1e-4f)
		ahead = QVector3D(0.f, 0.f, -1.f); // looking straight up or down
	ahead.normalize();
	panelPose.position = toXr(toQt(head.position) + panelDistance * ahead - QVector3D(0.f, panelDrop, 0.f));
	panelPose.orientation = toXr(QQuaternion::rotationTo(QVector3D(0.f, 0.f, 1.f), -ahead));
	panelPlacePending = false;
}

void OpenXR::placeArrow(const XrPosef& head)
{
	arrowVisible = false;
	const QList<StelObjectP> selected = GETSTELMODULE(StelObjectMgr)->getSelectedObject();
	if (!arrowPending || selected.isEmpty())
	{
		arrowPending = false;
		return;
	}
	const QVector3D target = fromAltAz(selected.first()->getAltAzPosAuto(StelApp::getInstance().getCore()), yaw);
	const QQuaternion orientation = toQt(head.orientation);
	const QVector3D ahead = orientation.rotatedVector(QVector3D(0.f, 0.f, -1.f));
	if (QVector3D::dotProduct(ahead, target) > std::cos(arrowDoneDegrees / M_180_PI))
	{
		arrowPending = false; // found
		return;
	}
	// Turned in the view towards it; straight behind, it points to the side.
	QVector3D across = target - QVector3D::dotProduct(target, ahead) * ahead;
	if (across.lengthSquared() < 1e-6f)
		across = orientation.rotatedVector(QVector3D(1.f, 0.f, 0.f));
	across.normalize();
	const QVector3D toHead = -ahead;
	arrowPose.orientation = toXr(QQuaternion::fromAxes(across, QVector3D::crossProduct(toHead, across), toHead));
	arrowPose.position = toXr(toQt(head.position) + arrowDistance * ahead + 0.15f * across);
	arrowVisible = arrowQuad.swapchain != XR_NULL_HANDLE;
}

float OpenXR::handMenuMeters() const
{
	return handMenuWidth * static_cast<float>(menu.scale);
}

float OpenXR::cardMeters() const
{
	return static_cast<float>(2. * cardDistance * std::tan(0.5 * cardDegrees / M_180_PI));
}

void OpenXR::placeHandMenu(XrTime time)
{
	const bool wasShown = handMenuShown;
	handMenuShown = false;
	XrSpaceLocation hand{XR_TYPE_SPACE_LOCATION};
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if (!handMenuVisible || !handMenuQuad.swapchain || !aimSpaces[leftHand]
	    || !XR_SUCCEEDED(xrLocateSpace(aimSpaces[leftHand], localSpace, time, &hand)) || (hand.locationFlags & valid) != valid)
		return;
	// The aim's -z points where the controller does and +y is its top; the
	// panel faces back along it, at the head, leaning back about its x axis.
	const QQuaternion aim = toQt(hand.pose.orientation);
	const QQuaternion orientation = aim * QQuaternion::fromAxisAndAngle(0.f, 1.f, 0.f, handMenuSwingDegrees)
					* QQuaternion::fromAxisAndAngle(1.f, 0.f, 0.f, -handMenuTiltDegrees);
	const QSize size = OpenXRMenu::size(OpenXRMenu::Hand);
	const float height = handMenuMeters() * size.height() / size.width();
	const QVector3D bottom = toQt(hand.pose.position) + aim.rotatedVector(QVector3D(0.f, handMenuAbove, -handMenuAhead));
	const QVector3D center = bottom + 0.5f * height * orientation.rotatedVector(QVector3D(0.f, 1.f, 0.f));
	const double dt = handMenuFollowedAt ? qBound(0., (time - handMenuFollowedAt) * 1e-9, 0.1) : 1.;
	handMenuFollowedAt = time;
	const double follow = 1. - std::exp(-dt / (handHover >= 0 ? handMenuAimedFollowSeconds : handMenuFollowSeconds));
	const bool jump = !wasShown;
	const QVector3D position = jump ? center : toQt(handMenuPose.position) + float(follow) * (center - toQt(handMenuPose.position));
	const QQuaternion turned = jump ? orientation : QQuaternion::slerp(toQt(handMenuPose.orientation), orientation, float(follow));
	handMenuPose = {toXr(turned.normalized()), toXr(position)};

	const QVector3D facing = turned.rotatedVector(QVector3D(0.f, 0.f, 1.f));
	handMenuShown = QVector3D::dotProduct(facing, (toQt(headPose.position) - position).normalized()) > handMenuFacingAway;
	if (handMenuShown && !wasShown)
		menuClock.invalidate(); // drawn afresh as it shows
}

void OpenXR::placeCard()
{
	cardShown = loupeShown = false;
	const QList<StelObjectP> selected = GETSTELMODULE(StelObjectMgr)->getSelectedObject();
	if (selected.isEmpty())
		cardObject.clear();
	// The loupe shows the object picked, or nothing.
	if (selected.isEmpty() || selected.first() != loupeObject)
		loupeFov = 0.;
	if (selected.isEmpty() || !cardQuad.swapchain)
		return;
	// Beside the object, on its right as the head sees it; the loupe on its left.
	const QVector3D target = fromAltAz(selected.first()->getAltAzPosAuto(StelApp::getInstance().getCore()), yaw);
	QVector3D right = toQt(headPose.orientation).rotatedVector(QVector3D(1.f, 0.f, 0.f));
	right -= QVector3D::dotProduct(right, target) * target;
	if (right.lengthSquared() < 1e-6f)
		return; // straight overhead of the head's right: nowhere to put it
	right.normalize();
	const auto beside = [&](double degrees, float side) {
		const double offset = (0.5 * degrees + cardGapDegrees) / M_180_PI;
		return toQt(headPose.position)
		       + cardDistance * (target * float(std::cos(offset)) + side * right * float(std::sin(offset))).normalized();
	};
	const QVector3D center = beside(cardDegrees, 1.f);
	cardPose = {toXr(facingHead(center, headPose)), toXr(center)};
	cardShown = true;
	if (loupeFov > 0.)
	{
		// Its view looks at the object, upright as the eyes' views are.
		const QVector3D at = beside(loupeDegrees, -1.f);
		loupePose = {toXr(facingHead(at, headPose)), toXr(at)};
		const QVector3D up = toQt(headPose.orientation).rotatedVector(QVector3D(0.f, 1.f, 0.f));
		loupeTurn = QQuaternion::fromDirection(-target, up);
		loupeShown = loupe.swapchain != XR_NULL_HANDLE;
	}
	// Where it went, for tests that point at it (xrctl's yaw and pitch).
	if (selected.first() != cardObject)
	{
		cardObject = selected.first();
		const QVector3D d = (center - toQt(headPose.position)).normalized();
		qDebug().nospace() << "OpenXR: card for " << cardObject->getEnglishName() << " at yaw "
				   << qRound(std::atan2(d.x(), -d.z()) * M_180_PI) << " pitch "
				   << qRound(std::asin(d.y()) * M_180_PI);
	}
}

void OpenXR::watchTime()
{
	// Running on, the time moves by the rate; anything more is a change, with
	// some slack for the frames' uneven timing.
	const StelCore* core = StelApp::getInstance().getCore();
	const double jd = core->getJD(), rate = core->getTimeRate();
	const qint64 now = clock.nsecsElapsed();
	if (timeWatched)
	{
		const double expected = watchedJD + rate * (now - watchedAt) * 1e-9;
		const double slack = qMax(2. * StelCore::JD_SECOND, std::fabs(rate) * 0.1);
		if (std::fabs(jd - expected) > slack || rate != watchedRate)
			timeChangedAt = now;
	}
	timeWatched = true;
	watchedJD = jd;
	watchedRate = rate;
	watchedAt = now;
	timeBarShown = timeChangedAt && now - timeChangedAt < timeBarShowNs;
}

void OpenXR::renderPanel()
{
	if (!running || !panelVisible)
		return;
	// Everything in the scene but the sky, which only draws into the window.
	StelMainView& view = StelMainView::getInstance();
	const QSize size = view.viewport()->size();
	QImage image(size * panelScale, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::TextAntialiasing);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	view.scene()->render(&painter, QRectF(), QRectF(QPointF(0., 0.), size));
	painter.end();
	panelImage = image;
	panelImageNew = true;
	// Again, as the clock and the info text change: rendering the GUI and
	// handing it on costs several ms, so quickly only while the pointer is on
	// it. The scene says nothing of its changes with the window not painted.
	panelTimer.start(panelInput.isValid() && panelInput.elapsed() < 1000 ? 50 : 500);
}

void OpenXR::drawView(Eye& target, uint32_t image, const XrFovf& fov, const XrQuaternionf& orientation,
		      const StelProjector::StelProjectorParams& baseParams, bool firstView)
{
	QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
	gl->glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
	gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.images[image], 0);
	// Some modules draw straight into the viewport as they find it, the Milky
	// Way for one; the first view would get the window's, cutting it off.
	gl->glViewport(0, 0, target.width, target.height);
	StelCore* core = StelApp::getInstance().getCore();
	core->setCurrentStelProjectorParams(eyeParams(baseParams, fov, target.width, target.height));
	core->setMatAltAzModelView(modelView(orientation, yaw));
	// The atmosphere works out its colors in update(), on a grid over the view
	// it is drawn in, so each view needs its own.
	qint64 start = clock.nsecsElapsed();
	GETSTELMODULE(LandscapeMgr)->update(0.);
	timing.atmosphere += clock.nsecsElapsed() - start;
	// It reports how bright its part of the sky is too, to which the eye
	// adapts. The first view of a frame sets the adaptation for all.
	if (!firstView)
		core->getSkyDrawer()->keepAdaptation();
	start = clock.nsecsElapsed();
	StelApp::getInstance().draw();
	timing.draws += clock.nsecsElapsed() - start;
}

bool OpenXR::drawLoupe(const StelProjector::StelProjectorParams& baseParams)
{
	uint32_t index = 0;
	XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	imageWait.timeout = XR_INFINITE_DURATION;
	if (!ok(xrAcquireSwapchainImage(loupe.swapchain, nullptr, &index), "xrAcquireSwapchainImage(loupe)")
	    || !ok(xrWaitSwapchainImage(loupe.swapchain, &imageWait), "xrWaitSwapchainImage(loupe)"))
		return false;
	const float half = static_cast<float>(0.5 * loupeFov / M_180_PI);
	const XrFovf fov{-half, half, half, -half};
	// Fainter stars come out as the field narrows, as through a telescope.
	StelCore* core = StelApp::getInstance().getCore();
	StelMovementMgr* mmgr = core->getMovementMgr();
	StelSkyDrawer* drawer = core->getSkyDrawer();
	const double eyeFov = mmgr->getCurrentFov();
	mmgr->setFov(loupeFov);
	drawer->update(0.);
	loupeLimit = drawer->getLimitMagnitude();
	// As the first view it sets the adaptation the eyes keep; its own bright
	// objects, the Moon filling it, would make the eyes' sky dark the next
	// frame, so it reports none. Quietly, as the setting itself stays.
	const bool adapting = drawer->getFlagLuminanceAdaptation();
	const auto setAdapting = [drawer](bool b) {
		const QSignalBlocker quiet(drawer);
		drawer->setFlagLuminanceAdaptation(b);
	};
	setAdapting(false);
	drawView(loupe, index, fov, toXr(loupeTurn), baseParams, true);
	setAdapting(adapting);
	cutEyepiece();
	mmgr->setFov(eyeFov);
	drawer->update(0.);
	return true; // released with the eyes
}

GLuint OpenXR::screenQuad()
{
	if (!quadVao)
	{
		QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
		const GLfloat corners[] = {-1, -1, 1, -1, -1, 1, 1, 1};
		gl->glGenVertexArrays(1, &quadVao);
		gl->glBindVertexArray(quadVao);
		gl->glGenBuffers(1, &quadVbo);
		gl->glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
		gl->glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
		gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
		gl->glEnableVertexAttribArray(0);
		gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
		gl->glBindVertexArray(0);
	}
	return quadVao;
}

void OpenXR::cutEyepiece()
{
	// Vertex arrays, which the core profile needs to draw at all, come with GL 3.
	if (!StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		return;
	QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
	if (!eyepieceProgram)
	{
		eyepieceProgram = std::make_unique<QOpenGLShaderProgram>();
		eyepieceProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE vec2 vertex;
VARYING vec2 position;
void main()
{
	position = vertex;
	gl_Position = vec4(vertex, 0., 1.);
}
)");
		eyepieceProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + R"(
VARYING vec2 position;
uniform float radius;
uniform float pixel;
void main()
{
	FRAG_COLOR = vec4(clamp((radius - length(position)) / pixel + 0.5, 0., 1.));
}
)");
		eyepieceProgram->bindAttributeLocation("vertex", 0);
		if (!eyepieceProgram->link())
			qWarning() << "OpenXR: loupe eyepiece shader:" << eyepieceProgram->log();
	}
	if (!eyepieceProgram->isLinked())
		return;

	gl->glBindFramebuffer(GL_FRAMEBUFFER, loupe.fbo);
	gl->glViewport(0, 0, loupe.width, loupe.height);
	gl->glDisable(GL_DEPTH_TEST);
	// Opaque inside, whatever alpha the sky left, then everything times the
	// eyepiece's coverage: premultiplied, and clear around it.
	gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	gl->glClearColor(0.f, 0.f, 0.f, 1.f);
	gl->glClear(GL_COLOR_BUFFER_BIT);
	gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl->glEnable(GL_BLEND);
	gl->glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	eyepieceProgram->bind();
	eyepieceProgram->setUniformValue("radius", eyepieceRadius);
	eyepieceProgram->setUniformValue("pixel", 2.f / loupe.width);
	gl->glBindVertexArray(screenQuad());
	gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gl->glBindVertexArray(0);
	eyepieceProgram->release();
	gl->glDisable(GL_BLEND);
}

void OpenXR::setRunning(bool b)
{
	if (b == running)
		return;
	running = b;
	StelCore* core = StelApp::getInstance().getCore();
	QSettings* conf = StelApp::getInstance().getSettings();
	if (b)
	{
		windowParams = core->getCurrentStelProjectorParams();
		// A coarser grid for the atmosphere's colors, worked out on the CPU for
		// every view: a smooth gradient, it hardly shows.
		atmosphereRows = conf->value(atmosphereRowsKey, 44);
		conf->setValue(atmosphereRowsKey, 24);
	}
	else
	{
		core->setCurrentStelProjectorParams(windowParams);
		conf->setValue(atmosphereRowsKey, atmosphereRows);
	}
	// The headset paces the frames in xrWaitFrame, from a timer of their own:
	// painting the window, which nobody sees in a headset, only slows them.
	StelMainView::getInstance().setFramesRunExternally(b);
	// The head moves the view by fractions of a pixel all the time, which
	// labels snapped to whole pixels show as jiggling against the stars.
	StelPainter::setTextPixelSnapping(!b);
	// Nor paint it at all: with an object selected, painting the GUI sets the
	// info text, which changes the scene, which paints it again, nonstop. The
	// GUI panel renders the scene itself.
	StelMainView::getInstance().viewport()->setUpdatesEnabled(!b);
	if (b)
		frameTimer.start();
	else
		frameTimer.stop();
}

void OpenXR::finishDrawing()
{
	// SteamVR on the Frame hands the images on to Vulkan through Zink; one
	// released before its drawing is done can show half drawn: the right eye,
	// drawn last. STELLARIUM_XR_SYNC=flush or none, to compare.
	QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
	if (sync == Sync::Finish)
		gl->glFinish();
	else if (sync == Sync::Flush)
		gl->glFlush();
}

void OpenXR::pulse(float amplitude, int milliseconds)
{
	XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = hapticAction;
	info.subactionPath = handPaths[activeHand];
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
	vibration.amplitude = amplitude;
	vibration.duration = milliseconds * 1000000LL;
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	xrApplyHapticFeedback(session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

bool OpenXR::createQuad(Quad& quad, int width, int height, bool isStatic)
{
	if (quad.swapchain)
		xrDestroySwapchain(quad.swapchain);
	quad = Quad();
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.createFlags = isStatic ? XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT : 0;
	info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = static_cast<uint32_t>(width);
	info.height = static_cast<uint32_t>(height);
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!ok(xrCreateSwapchain(session, &info, &quad.swapchain), "xrCreateSwapchain(quad)"))
		return false;
	quad.width = width;
	quad.height = height;
	return true;
}

void OpenXR::uploadQuad(Quad& quad, const QImage& image)
{
	uint32_t index = 0, count = 0;
	XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	if (!ok(xrAcquireSwapchainImage(quad.swapchain, nullptr, &index), "xrAcquireSwapchainImage(quad)"))
		return;
	xrEnumerateSwapchainImages(quad.swapchain, 0, &count, nullptr);
	std::vector<XrSwapchainImageOpenGLKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
	xrEnumerateSwapchainImages(quad.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
	if (ok(xrWaitSwapchainImage(quad.swapchain, &wait), "xrWaitSwapchainImage(quad)") && index < count)
	{
		// GL images start at the bottom row, QImage at the top one.
		const QImage rows = image.mirrored(false, true);
		QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
		GLint previous = 0;
		gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
		gl->glBindTexture(GL_TEXTURE_2D, images[index].image);
		gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, quad.width, quad.height, GL_RGBA, GL_UNSIGNED_BYTE, rows.constBits());
		gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
	}
	finishDrawing();
	ok(xrReleaseSwapchainImage(quad.swapchain, nullptr), "xrReleaseSwapchainImage(quad)");
}
