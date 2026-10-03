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

#ifndef OPENXR_HPP
#define OPENXR_HPP

#include "StelModule.hpp"
#include "StelObjectType.hpp"
#include "StelPluginInterface.hpp"
#include "StelProjector.hpp"
#include "OpenXRMenu.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QOpenGLShaderProgram>
#include <QImage>
#include <QPointF>
#include <QQuaternion>
#include <QVector3D>
#include <QSize>
#include <QTimer>
#include <QVariant>
#include <qopengl.h>
#include <openxr/openxr.h>
#include <memory>
#include <utility>
#include <vector>


//! @class OpenXR
//! Shows the sky in a VR headset through OpenXR, e.g. the Steam Frame under SteamVR.
//! The head turns the view, and each eye gets its own field of view.
//!
//! It runs without keyboard or mouse. A controller points, and its trigger
//! clicks; the controller pulses when the pointer reaches something to click,
//! and on a click. The menu floats over the left hand, a category at a time; a
//! card on the object picked floats in the sky beside it. Every other button
//! does one thing of its own (see Button). A menu tile or the left view button opens
//! the full GUI on a bigger panel, which takes the pointer like a mouse.
//!
//! Pointed at the sky, the trigger picks the object there and the right
//! stick's left and right scrub time. The window is not painted meanwhile;
//! Space recenters too.
//!
//! The sky itself is never magnified, so the horizon and the stars around
//! stay in view and the head's turns move it as ever; the loupe is the
//! telescope. Y opens a round view of the object picked, magnified, pinned in
//! the sky on its left as the card is on its right, and closes it again.
//! While it is open the right stick's up and down narrow or widen its field,
//! and the trigger picks what it shows; another object picked there is the
//! one it shows. It closes too as the object is no longer picked.
//!
//! Everything in the sky is at infinity, so both eyes see it from the same point:
//! the view is not stereoscopic, and the Scenery3D plugin cannot be used. So the
//! sky is drawn once, over both eyes' fields of view, and goes to both; the
//! menus and pointer float over it, each eye getting its own view of
//! them from the runtime.
//!
//! The session uses the app's OpenGL context through GLX (XR_KHR_opengl_enable),
//! so this runs on Linux under X11 or Xwayland only.
class OpenXR : public StelModule
{
	Q_OBJECT

public:
	OpenXR();
	~OpenXR() override;

	void init() override;
	void deinit() override;

public slots:
	void recenter();

private:
	struct Eye
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		std::vector<GLuint> images;
		int width = 0, height = 0;
		GLuint fbo = 0, depthStencil = 0;
	};
	//! An image shown flat in the room: the GUI panel or the pointer's cursor.
	struct Quad
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		int width = 0, height = 0;
	};

	//! Replaces StelApp::draw(): runs one OpenXR frame, or draws the window alone.
	void drawFrame();
	bool start();
	//! Ends OpenXR for good; the window goes on drawing the sky.
	void stop();
	void pollEvents();
	//! Draws both eyes and fills in the layer's views; false if nothing was drawn.
	bool drawEyes(XrTime time, XrCompositionLayerProjectionView* layerViews);
	bool ok(XrResult result, const char* what) const;

	bool createActions();
	bool suggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings);
	//! Turns the controllers into mouse events on the panel, or picks in the sky.
	void handleInput(XrTime time);
	//! Where the ray from origin along dir meets a quad of this size in pixels,
	//! as (0, 0) at its top left to (1, 1) at its bottom right.
	static bool hitQuad(const XrPosef& pose, float width, const QSize& size, const QVector3D& origin,
			    const QVector3D& dir, QPointF* uv, float* distance);
	//! Puts the GUI panel and the menu in front of the head.
	void placePanel(const XrPosef& head);
	//! Sets the "Show me" arrow for this frame, hidden once the object is in view.
	void placeArrow(const XrPosef& head);
	//! Puts the menu over the left hand.
	void placeHandMenu(XrTime time);
	//! What each button does, on its press, from that hand.
	void press(int button, int hand);
	//! Puts the card, and the loupe if open, in the sky beside the object picked, if any.
	void placeCard();
	//! Shows the time bar for a while after the time changes.
	void watchTime();
	float handMenuMeters() const;
	float cardMeters() const;
	bool createEye(Eye& e, int width, int height);
	//! The one view both eyes get, as wide as both together.
	bool createSharedView(const XrView* views);
	//! Draws the sky into one image of a view: an eye's, or the loupe's.
	void drawView(Eye& target, uint32_t image, const XrFovf& fov, const XrQuaternionf& orientation,
		      const StelProjector::StelProjectorParams& baseParams, bool firstView);
	//! The loupe's view of the object, its image left for drawEyes() to
	//! release; false if nothing was drawn.
	bool drawLoupe(const StelProjector::StelProjectorParams& baseParams);
	//! Makes the loupe's view round: clear outside the eyepiece.
	void cutEyepiece();
	//! Two triangles over the whole viewport, for cutEyepiece().
	GLuint screenQuad();
	//! Starts or stops running the frames from frameTimer.
	void setRunning(bool b);
	//! Waits for, or at least sends off, the drawing of an image before it goes
	//! to the runtime, as STELLARIUM_XR_SYNC says.
	void finishDrawing();
	//! Vibrates the pointing controller.
	void pulse(float amplitude, int milliseconds);
	//! In meters, from the GUI's font size: see textDegrees.
	float panelWidth() const;
	//! Draws the GUI into panelImage, outside the paint of the window.
	void renderPanel();
	bool createQuad(Quad& quad, int width, int height, bool isStatic);
	void uploadQuad(Quad& quad, const QImage& image);

	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction aimAction = XR_NULL_HANDLE, triggerAction = XR_NULL_HANDLE;
	XrAction stickAction = XR_NULL_HANDLE, hapticAction = XR_NULL_HANDLE;
	//! The Frame's buttons; other controllers get what they have of them.
	//! Right: A pauses, B deselects, X goes back to now, Y opens or closes the
	//! loupe on the object picked, the menu button
	//! shows or hides the menu, the bumper and grip switch constellation lines
	//! and art, the stick's click recenters.
	//! Left: the d-pad's up and down speed time up and slow it down,
	//! left and right step it by an hour, the view button opens or closes the
	//! full GUI, the bumper and grip switch the ground and the atmosphere, the
	//! stick's click recenters, and flicks of the stick go through the menu.
	enum Button { A, B, X, Y, Menu, View, Up, Down, Left, Right, Bumper, Grip, Stick, Buttons };
	XrAction buttonActions[Buttons] = {};
	bool buttonDown[Buttons][2] = {};
	XrPath handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrSpace aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	bool frameControllerExtension = false;
	bool triggerDown[2] = {false, false};
	int activeHand = 1;
	bool aimValid = false;
	XrTime lastInputTime = 0;
	//! What a click would hit last frame, for the haptic tick on reaching another.
	quintptr lastTarget = 0;
	//! The loupe's field across in degrees, 0 when closed, and the object it
	//! shows; its view's orientation in the sky, and the faintest it draws.
	double loupeFov = 0.;
	StelObjectP loupeObject;
	bool loupeShown = false, loupeDrawn = false;
	XrPosef loupePose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	QQuaternion loupeTurn;
	float loupeLimit = 0.f;
	std::unique_ptr<QOpenGLShaderProgram> eyepieceProgram;
	GLuint quadVao = 0, quadVbo = 0;

	Quad panel, cursor, skyCursor, handMenuQuad, cardQuad, timeBarQuad, laserQuad, arrowQuad, loupeRim;
	//! Points the way to the object picked, after "Show me", until it is in view.
	XrPosef arrowPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	bool arrowPending = false, arrowVisible = false;
	//! The pointer's beam: turned about its length to face the head.
	XrPosef laserPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	float laserLength = 0.f;
	OpenXRMenu menu;
	//! Shown and hidden with the menu button. The left stick, flicked, goes to
	//! the next category or page.
	bool handMenuVisible = true, handMenuShown = false, flickArmed = true;
	XrPosef handMenuPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	XrTime handMenuFollowedAt = 0;
	bool cardShown = false;
	StelObjectP cardObject;
	XrPosef cardPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	int handHover = -1, cardHover = -1, drawnHandHover = -1, drawnCardHover = -1;
	QElapsedTimer menuClock;
	bool timeWatched = false, timeBarShown = false;
	double watchedJD = 0., watchedRate = 0.;
	qint64 watchedAt = 0, timeChangedAt = 0;
	XrPosef headPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	QImage panelImage;
	bool panelImageNew = false;
	bool panelVisible = false;
	bool panelPlacePending = true;
	XrPosef panelPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	//! Redraws the panel at most this often: the GUI changes whenever the info text does.
	QTimer panelTimer;
	//! Since the pointer last moved on the panel.
	QElapsedTimer panelInput;
	bool cursorVisible = false, cursorInSky = false;
	XrPosef cursorPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
	float cursorSize = 0.f;
	QPointF lastMouse{-1., -1.};
	bool guiPressed = false;

	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
	Eye eyes[2], loupe;
	//! One view for both eyes, unless STELLARIUM_XR_VIEWS=eyes.
	Eye sky;
	XrFovf skyFov{};
	bool sharedView = true;
	//! The window's projection, back when the session ends: meanwhile the views' stays.
	StelProjector::StelProjectorParams windowParams;
	static constexpr const char* atmosphereRowsKey = "landscape/atmosphereybin";
	QVariant atmosphereRows;
	int64_t format = 0; //!< of every swapchain
	bool failed = false;
	bool running = false;
	bool setUp = false; //!< Stellarium set for the headset, once per session
	QTimer frameTimer;
	enum class Sync { Finish, Flush, Off } sync = Sync::Finish;
	bool recenterPending = true;
	//! Turns the headset's forward direction to the azimuth the view had when recentered.
	double yaw = 0.;
	//! For the frame rate in the log, every 10 seconds.
	QElapsedTimer rateTimer;
	int framesDrawn = 0;
	//! What the frames spend their time on, in ns, logged with the frame rate.
	struct Timing
	{
		qint64 waiting = 0, outside = 0, atmosphere = 0, draws = 0, gpu = 0;
		int gpuFrames = 0;
		QHash<QString, qint64> updates;
	} timing;
	QElapsedTimer clock;
	qint64 lastFrameEnd = 0;
	GLuint gpuQueries[3] = {0, 0, 0};
	bool gpuQueryPending[3] = {false, false, false};
	int gpuQuery = 0;
};


//! This class is used by Qt to manage a plug-in interface
class OpenXRStelPluginInterface : public QObject, public StelPluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID StelPluginInterface_iid)
	Q_INTERFACES(StelPluginInterface)
public:
	StelModule* getStelModule() const override;
	StelPluginInfo getPluginInfo() const override;
};

#endif // OPENXR_HPP
