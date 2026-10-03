/*
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

#include "OpenXRMenu.hpp"
#include "ConstellationMgr.hpp"
#include "LandscapeMgr.hpp"
#include "NomenclatureItem.hpp"
#include "Planet.hpp"
#include "SolarSystem.hpp"
#include "StelActionMgr.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelLocaleMgr.hpp"
#include "StelLocationMgr.hpp"
#include "StelModuleMgr.hpp"
#include "StelObjectMgr.hpp"
#include "StelSkyCultureMgr.hpp"
#include "StelSkyDrawer.hpp"
#include "StelTranslator.hpp"
#include "StelUtils.hpp"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace
{
// Labels a little over Meta's comfortable 1° at the size OpenXR shows them,
// tile labels right at it, and tiles well over the 2-3° commonly asked of
// targets.
constexpr int fontPixels = 30, smallPixels = 26;
constexpr int margin = 16, gap = 10, columns = 3, tileWidth = 212, tileHeight = 104, iconSize = 40;
constexpr int titleHeight = 72, crumbHeight = 48, pagerHeight = 56, headingHeight = 40;
constexpr int handWidth = 2 * margin + columns * tileWidth + (columns - 1) * gap;
constexpr int crumbTop = margin + titleHeight + gap, contentTop = crumbTop + crumbHeight + gap;
constexpr int contentHeight = 4 * (tileHeight + gap) + 2 * headingHeight;
constexpr int pagerTop = contentTop + contentHeight + gap, handHeight = pagerTop + pagerHeight + margin;
constexpr int cardWidth = 600, cardLineHeight = 34, cardButtonHeight = 60;
constexpr int cardHeight = margin + 44 + 3 * cardLineHeight + gap + cardButtonHeight + margin;
constexpr int timeBarWidth = 720, timeBarHeight = 84;

const QColor panelColor(12, 16, 28, 225), tileColor(255, 255, 255, 36), onColor(35, 80, 150),
	dimText(200, 210, 230);

// Dark places around the world, and two odd ones, to see the sky turn with
// latitude. Given by coordinates, so they don't depend on the location list.
struct Place
{
	const char* name;
	const char* region;
	float longitude, latitude;
	int altitude;
	const char* timeZone;
	int bortle;
};
const Place places[] = {
	{N_("Mauna Kea"), "Northern America", -155.4681f, 19.8207f, 4205, "Pacific/Honolulu", 1},
	{N_("Atacama"), "South America", -70.4042f, -24.6272f, 2635, "America/Santiago", 1},
	{N_("La Palma"), "Southern Europe", -17.8816f, 28.7606f, 2396, "Atlantic/Canary", 2},
	{N_("Namibia"), "Southern Africa", 16.0f, -24.95f, 1000, "Africa/Windhoek", 1},
	{N_("Uluru"), "Australasia", 131.0369f, -25.3444f, 500, "Australia/Darwin", 1},
	{N_("Iceland"), "Northern Europe", -21.1299f, 64.2559f, 100, "Atlantic/Reykjavik", 2},
	{N_("North Pole"), "Northern Europe", 0.f, 90.f, 0, "UTC", 1},
	{N_("Equator"), "South America", -78.4678f, -0.1807f, 2850, "America/Guayaquil", 5},
};

const char* const worlds[] = {N_("Moon"), N_("Mercury"), N_("Venus"), N_("Mars"), N_("Jupiter"),
			      N_("Saturn"), N_("Uranus"), N_("Neptune"), N_("Pluto")};
const char* const planets[] = {N_("Sun"), N_("Moon"), N_("Mercury"), N_("Venus"), N_("Mars"),
			       N_("Jupiter"), N_("Saturn"), N_("Uranus"), N_("Neptune"), N_("Pluto")};
// The brightest, and a few well known for other reasons.
const char* const stars[] = {"Sirius", "Canopus", "Rigil Kentaurus", "Arcturus", "Vega", "Capella",
			     "Rigel", "Procyon", "Achernar", "Betelgeuse", "Hadar", "Altair",
			     "Acrux", "Aldebaran", "Antares", "Spica", "Pollux", "Fomalhaut",
			     "Deneb", "Mimosa", "Regulus", "Castor", "Polaris", "Mizar", "Algol"};

StelLocation placeLocation(const Place& place)
{
	return StelLocation(place.name, QString(), place.region, place.longitude, place.latitude, place.altitude, 0,
			    place.timeZone, place.bortle);
}

//! The same icon as the GUI's button, from its resources: ":/graphicGui/<name>-on.png",
//! or a resource path of its own.
const QImage& tileIcon(const QString& name)
{
	static QHash<QString, QImage> icons;
	static const QImage none;
	if (name.isEmpty())
		return none;
	auto it = icons.find(name);
	if (it == icons.end())
		it = icons.insert(name, QImage(name.startsWith(':') ? name : QString(":/graphicGui/%1-on.png").arg(name))
					     .scaled(iconSize, iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	return *it;
}

QString localTime(double jd)
{
	const StelCore* core = StelApp::getInstance().getCore();
	return StelApp::getInstance().getLocaleMgr().getPrintableTimeLocal(jd, core->getUTCOffset(jd)).remove(5, 3); // the seconds, keeping any AM/PM
}

StelAction* findAction(const char* id)
{
	return StelApp::getInstance().getStelActionManager()->findAction(id);
}

void trigger(const char* id)
{
	if (StelAction* a = findAction(id))
		a->trigger();
}

void togglePause()
{
	trigger(StelApp::getInstance().getCore()->getTimeRate() == 0. ? "actionSet_Real_Time_Speed" : "actionSet_Time_Rate_Zero");
}

bool paused()
{
	return StelApp::getInstance().getCore()->getTimeRate() == 0.;
}

using Item = OpenXRMenu::Item;
using Section = OpenXRMenu::Section;

//! A tile that switches a StelAction on and off.
Item toggle(const char* label, const char* action, const QString& icon = QString())
{
	Item item{q_(label), icon};
	item.on = [action] { const StelAction* a = findAction(action); return a && a->isChecked(); };
	item.run = [action] { if (StelAction* a = findAction(action)) a->setChecked(!a->isChecked()); };
	return item;
}

Item command(const QString& label, std::function<void()> run, const QString& icon = QString(),
	     std::function<bool()> on = nullptr)
{
	Item item{label, icon};
	item.run = std::move(run);
	item.on = std::move(on);
	return item;
}

Item stepper(const QString& label, std::function<QString()> value, std::function<void(int)> step)
{
	Item item{label, QString()};
	item.value = std::move(value);
	item.step = std::move(step);
	return item;
}

//! The local date and time at jd, and back.
struct LocalDate
{
	int year, month, day, hour, minute, second;
	static LocalDate at(double jd)
	{
		LocalDate d;
		const double offset = StelApp::getInstance().getCore()->getUTCOffset(jd) / 24.;
		StelUtils::getDateTimeFromJulianDay(jd + offset, &d.year, &d.month, &d.day, &d.hour, &d.minute, &d.second);
		return d;
	}
	double jd() const
	{
		double local = 0.;
		StelUtils::getJDFromDate(&local, year, month, qMin(day, StelUtils::numberOfDaysInMonthInYear(month, year)), hour,
					 minute, second);
		return local - StelApp::getInstance().getCore()->getUTCOffset(local) / 24.;
	}
};

Item dateStepper(const char* label, int LocalDate::*field)
{
	return stepper(q_(label),
		       [field] {
			       const LocalDate d = LocalDate::at(StelApp::getInstance().getCore()->getJD());
			       return QString::number(d.*field).rightJustified(field == &LocalDate::year ? 1 : 2, '0');
		       },
		       [field](int by) {
			       StelCore* core = StelApp::getInstance().getCore();
			       LocalDate d = LocalDate::at(core->getJD());
			       d.*field += by;
			       // Carried by hand only where the calendar doesn't: months into years.
			       if (d.month < 1)
				       d.month = 12, --d.year;
			       else if (d.month > 12)
				       d.month = 1, ++d.year;
			       if (field == &LocalDate::day || field == &LocalDate::hour || field == &LocalDate::minute)
				       core->setJD(core->getJD() + by * (field == &LocalDate::day ? 1. : field == &LocalDate::hour ? 1. / 24. : 1. / 1440.));
			       else
				       core->setJD(d.jd());
		       });
}

Item timeStepper(const char* label, const char* back, const char* forward, std::function<QString()> value)
{
	return stepper(q_(label), std::move(value), [back, forward](int by) { trigger(by < 0 ? back : forward); });
}

//! Moves to the object's place in the sky: the headset turns there itself.
Item findItem(const QString& label, const QString& name, const std::function<void()>& showMe)
{
	Item item{label, QString()};
	item.run = [name, showMe] {
		if (GETSTELMODULE(StelObjectMgr)->findAndSelect(name) && showMe)
			showMe();
	};
	item.on = [name] {
		const QList<StelObjectP> selected = GETSTELMODULE(StelObjectMgr)->getSelectedObject();
		return !selected.isEmpty() && selected.first()->getEnglishName() == name;
	};
	return item;
}

//! Whether a tile takes two columns: the branches out of a category, which
//! have an icon or what they are set to next to their label.
bool wide(const Item& item)
{
	return item.open && (item.value || !item.icon.isEmpty());
}

//! Continents, from the region names, which are the UN's.
QString continentOf(const QString& region)
{
	for (const char* name : {"Africa", "Asia", "Europe"})
		if (region.contains(name))
			return name;
	if (region.contains("America") || region == "Caribbean")
		return "Americas";
	return "Oceania";
}
}

//! What a drawing of the menu is made of. Laid out on every call, and drawn only
//! when its description, which is what the menu shows, changed.
class OpenXRMenu::Canvas
{
public:
	enum Kind { Box, Text, Icon };
	struct Op
	{
		Kind kind;
		QRect rect;
		QColor color;
		QString text;
		int flags = 0, pixels = fontPixels;
		bool bold = false;
	};
	QList<Op> ops;
	QList<Target> targets;

	void box(const QRect& rect, const QColor& color, bool hovered)
	{
		ops.append({Box, rect, color, QString(), hovered});
	}
	void text(const QRect& rect, const QString& text, int flags, int pixels = fontPixels, const QColor& color = Qt::white,
		  bool bold = false)
	{
		ops.append({Text, rect, color, text, flags, pixels, bold});
	}
	void icon(const QRect& rect, const QString& name) { ops.append({Icon, rect, QColor(), name}); }
	//! A button: a box that can be pointed at and clicked.
	bool target(const QRect& rect, std::function<void()> act, bool on, int hoverTile)
	{
		const bool hovered = targets.size() == hoverTile;
		targets.append({rect, std::move(act)});
		box(rect, on ? onColor : tileColor, hovered);
		return hovered;
	}

	QString state() const
	{
		QString s;
		for (const Op& op : ops)
			s += QString("%1,%2,%3,%4,%5,%6,%7,%8,%9|").arg(op.kind).arg(op.rect.x()).arg(op.rect.y()).arg(op.rect.width())
				     .arg(op.color.rgba()).arg(op.flags).arg(op.pixels).arg(op.bold).arg(op.text);
		return s;
	}

	void draw(QPainter& painter) const
	{
		QFont font = QGuiApplication::font();
		for (const Op& op : ops)
		{
			switch (op.kind)
			{
				case Box:
					painter.setBrush(op.color);
					painter.setPen(op.flags ? QPen(Qt::white, 4) : QPen(Qt::NoPen));
					painter.drawRoundedRect(op.rect, 14, 14);
					break;
				case Text:
				{
					font.setPixelSize(op.pixels);
					font.setBold(op.bold);
					// Wrapped labels shrink a little rather than lose letters.
					if (op.flags & Qt::TextWordWrap)
					{
						const auto fits = [&op](const QFont& f) {
							const QFontMetrics m(f);
							for (const QString& word : op.text.split(' '))
								if (m.horizontalAdvance(word) > op.rect.width())
									return false;
							return m.boundingRect(op.rect, op.flags, op.text).height() <= op.rect.height();
						};
						while (font.pixelSize() > 18 && !fits(font))
							font.setPixelSize(font.pixelSize() - 1);
					}
					painter.setFont(font);
					painter.setPen(op.color);
					const QString text = op.flags & Qt::TextWordWrap
								     ? op.text
								     : QFontMetrics(font).elidedText(op.text, Qt::ElideRight, op.rect.width());
					painter.drawText(op.rect, op.flags, text);
					break;
				}
				case Icon:
				{
					const QImage& image = tileIcon(op.text);
					const QSize s = image.size().scaled(op.rect.size(), Qt::KeepAspectRatio);
					painter.drawImage(QRect(op.rect.center() - QPoint(s.width() / 2, s.height() / 2), s), image);
					break;
				}
			}
		}
	}
};

OpenXRMenu::OpenXRMenu()
{
	categories = {
		{N_("Sky"), "bbtSky", [this] { return skyContent(); }},
		{N_("Constellations"), "btConstellationLines", [this] { return constellationContent(); }},
		{N_("Lines and grids"), "btEquatorialGrid", [this] { return linesContent(); }},
		{N_("Landscape"), "btGround", [this] { return landscapeContent(); }},
		{N_("Time"), "bbtTime", [this] { return timeContent(); }},
		{N_("Location"), "bbtLocation", [this] { return locationContent(); }},
		{N_("Find"), "bbtSearch", [this] { return findContent(); }},
		{N_("View"), "bbtSettings", [this] { return viewContent(); }},
	};
}

QSize OpenXRMenu::size(Surface surface)
{
	return surface == Hand ? QSize(handWidth, handHeight) : QSize(cardWidth, cardHeight);
}

int OpenXRMenu::tileAt(Surface surface, const QPointF& pixel) const
{
	for (int i = 0; i < targets[surface].size(); ++i)
		if (targets[surface][i].rect.contains(pixel.toPoint()))
			return i;
	return -1;
}

std::function<void()> OpenXRMenu::actionFor(Surface surface, int tile) const
{
	return tile >= 0 && tile < targets[surface].size() ? targets[surface][tile].act : nullptr;
}

void OpenXRMenu::changed()
{
	for (QString& state : drawnState)
		state.clear();
}

bool OpenXRMenu::hasCard()
{
	return !GETSTELMODULE(StelObjectMgr)->getSelectedObject().isEmpty();
}

void OpenXRMenu::turnCategory(int by)
{
	category = (category + by + categories.size()) % categories.size();
	categoryPage = 0;
	path.clear();
	changed();
}

OpenXRMenu::Item OpenXRMenu::branch(const QString& label, const QString& icon, std::function<QList<Section>()> open,
				    std::function<QString()> value)
{
	Item item{label, icon};
	item.open = std::move(open);
	item.value = std::move(value);
	return item;
}

QList<Section> OpenXRMenu::skyContent()
{
	StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	return {
		{q_("Stars and deep sky"),
		 {toggle(N_("Stars"), "actionShow_Stars"),
		  toggle(N_("Star names"), "actionShow_Stars_Labels"),
		  stepper(q_("Star size"), [drawer] { return QString::number(drawer->getRelativeStarScale(), 'f', 1); },
			  [drawer](int by) { drawer->setRelativeStarScale(qBound(0.1, drawer->getRelativeStarScale() + 0.1 * by, 5.)); }),
		  toggle(N_("Deep sky"), "actionShow_Nebulas", "btNebula"),
		  toggle(N_("Deep sky images"), "actionShow_DSO_Textures", "btNebulaeBackground"),
		  toggle(N_("Milky Way"), "actionShow_MilkyWay")}},
		{q_("Solar system"),
		 {toggle(N_("Planets"), "actionShow_Planets", "btPlanets"),
		  toggle(N_("Planet names"), "actionShow_Planets_Labels"),
		  toggle(N_("Orbits"), "actionShow_Planets_Orbits"),
		  toggle(N_("Trails"), "actionShow_Planets_Trails"),
		  toggle(N_("Bigger Moon"), "actionShow_Planets_EnlargeMoon"),
		  toggle(N_("Minor bodies"), "actionShow_Planets_ShowMinorBodyMarkers")}},
		{q_("More"),
		 {toggle(N_("Zodiacal light"), "actionShow_ZodiacalLight"),
		  // Hidden, they are not computed either: about 6 ms a frame on the Frame.
		  toggle(N_("Satellites"), "actionShow_Satellite_Hints", ":/satellites/bt_satellites_on.png"),
		  toggle(N_("Meteor showers"), "actionShow_MeteorShowers", ":/MeteorShowers/btMS-on.png"),
		  toggle(N_("Exoplanets"), "actionShow_Exoplanets", ":/Exoplanets/btExoplanets-on.png")}},
	};
}

QList<Section> OpenXRMenu::constellationContent()
{
	ConstellationMgr* constellations = GETSTELMODULE(ConstellationMgr);
	StelSkyCultureMgr& cultures = StelApp::getInstance().getSkyCultureMgr();
	return {
		{q_("Show"),
		 {toggle(N_("Lines"), "actionShow_Constellation_Lines", "btConstellationLines"),
		  toggle(N_("Names"), "actionShow_Constellation_Labels", "btConstellationLabels"),
		  toggle(N_("Art"), "actionShow_Constellation_Art", "btConstellationArt"),
		  stepper(q_("Art brightness"), [constellations] { return QString::number(constellations->getArtIntensity(), 'f', 1); },
			  [constellations](int by) { constellations->setArtIntensity(qBound(0.f, constellations->getArtIntensity() + 0.1f * by, 1.f)); }),
		  toggle(N_("Boundaries"), "actionShow_Constellation_Boundaries", "btConstellationBoundaries"),
		  toggle(N_("Hulls"), "actionShow_Constellation_Hulls"),
		  toggle(N_("Asterisms"), "actionShow_Asterism_Lines", "btAsterismLines")}},
		{q_("Sky culture"),
		 {branch(q_("Sky culture"), "bbtSky",
			 [&cultures] {
				 QStringList names = cultures.getSkyCultureListI18();
				 names.sort(Qt::CaseInsensitive);
				 Section section{q_("Sky culture")};
				 for (const QString& name : names)
					 section.items << command(name, [&cultures, name] { cultures.setCurrentSkyCultureNameI18(name); },
								  QString(), [&cultures, name] { return cultures.getCurrentSkyCultureNameI18() == name; });
				 return QList<Section>{section};
			 },
			 [&cultures] { return cultures.getCurrentSkyCultureNameI18(); })}},
	};
}

QList<Section> OpenXRMenu::linesContent()
{
	return {
		{q_("Grids"),
		 {toggle(N_("Equatorial"), "actionShow_Equatorial_Grid", "btEquatorialGrid"),
		  toggle(N_("Azimuthal"), "actionShow_Azimuthal_Grid", "btAzimuthalGrid"),
		  toggle(N_("Ecliptic"), "actionShow_Ecliptic_Grid", "btEclipticGrid"),
		  toggle(N_("Galactic"), "actionShow_Galactic_Grid", "btGalacticGrid")}},
		{q_("Lines"),
		 {toggle(N_("Horizon"), "actionShow_Horizon_Line"),
		  toggle(N_("Meridian"), "actionShow_Meridian_Line"),
		  toggle(N_("Equator"), "actionShow_Equator_Line"),
		  toggle(N_("Ecliptic"), "actionShow_Ecliptic_Line"),
		  toggle(N_("Galactic equator"), "actionShow_Galactic_Equator_Line"),
		  toggle(N_("Precession circles"), "actionShow_Precession_Circles")}},
		{q_("Points"),
		 {toggle(N_("Cardinal points"), "actionShow_Cardinal_Points", "btCardinalPoints"),
		  toggle(N_("Celestial poles"), "actionShow_Celestial_Poles"),
		  toggle(N_("Equinoxes"), "actionShow_Equinox_Points"),
		  toggle(N_("Zenith and nadir"), "actionShow_Zenith_Nadir")}},
	};
}

QList<Section> OpenXRMenu::landscapeContent()
{
	LandscapeMgr* landscapes = GETSTELMODULE(LandscapeMgr);
	const StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	return {
		{q_("Show"),
		 {toggle(N_("Ground"), "actionShow_Ground", "btGround"),
		  toggle(N_("Atmosphere"), "actionShow_Atmosphere", "btAtmosphere"),
		  toggle(N_("Fog"), "actionShow_Fog"),
		  toggle(N_("Lights"), "actionShow_LandscapeIllumination")}},
		{q_("Light pollution"),
		 {timeStepper(N_("Faintest stars"), "actionShow_LightPollutionIncrease", "actionShow_LightPollutionReduce",
			      [drawer] { return QString::number(StelCore::luminanceToNELM(float(drawer->getLightPollutionLuminance())), 'f', 1); }),
		  toggle(N_("From location"), "actionShow_LightPollutionFromDatabase")}},
		{q_("Landscape"),
		 {branch(q_("Landscape"), "btGround",
			 [landscapes] {
				 QStringList names = landscapes->getAllLandscapeNames();
				 names.sort(Qt::CaseInsensitive);
				 Section section{q_("Landscape")};
				 for (const QString& name : names)
					 section.items << command(name, [landscapes, name] { landscapes->setCurrentLandscapeName(name); },
								  QString(), [landscapes, name] { return landscapes->getCurrentLandscapeName() == name; });
				 return QList<Section>{section};
			 },
			 [landscapes] { return landscapes->getCurrentLandscapeName(); })}},
	};
}

QList<Section> OpenXRMenu::timeContent()
{
	StelCore* core = StelApp::getInstance().getCore();
	const StelLocaleMgr& locale = StelApp::getInstance().getLocaleMgr();
	const auto date = [core, &locale] { return locale.getPrintableDateLocal(core->getJD(), core->getUTCOffset(core->getJD())); };
	// Today at that local hour: JD days start at noon, so local midnight is at .5.
	const auto today = [core](double hour) {
		return [core, hour] {
			const double jd = core->getJD(), offset = core->getUTCOffset(jd) / 24.;
			core->setJD(std::floor(jd + offset - 0.5) + 0.5 + hour / 24. - offset);
		};
	};
	// The Sun's rise and set nearest to now, at the usual -50' for refraction
	// and its disk; none where it stays up or down all day.
	const auto sun = [core](int which) {
		return [core, which] {
			const Vec4d rts = GETSTELMODULE(SolarSystem)->getSun()->getRTSTime(core, -0.8333);
			if (std::fabs(rts[3]) < 99.)
				core->setJD(rts[which]);
		};
	};
	return {
		{q_("Rate"),
		 {command(q_("Slower"), [] { trigger("actionDecrease_Time_Speed"); }, "btTimeRewind"),
		  command(q_("Pause"), togglePause, "btTimePause", paused),
		  command(q_("Faster"), [] { trigger("actionIncrease_Time_Speed"); }, "btTimeForward"),
		  command(q_("Real time"), [] { trigger("actionSet_Real_Time_Speed"); }, "btTimeRealtime",
			  [core] { return core->getTimeRate() == StelCore::JD_SECOND; })}},
		{q_("Step"),
		 {timeStepper(N_("Hour"), "actionSubtract_Solar_Hour", "actionAdd_Solar_Hour", [core] { return localTime(core->getJD()); }),
		  timeStepper(N_("Day"), "actionSubtract_Solar_Day", "actionAdd_Solar_Day", date),
		  timeStepper(N_("Week"), "actionSubtract_Solar_Week", "actionAdd_Solar_Week", date),
		  command(q_("Now"), [] { trigger("actionReturn_To_Current_Time"); }, "btTimeNow")}},
		{q_("Jump to"),
		 {command(q_("Tonight 22:00"), today(22)),
		  command(q_("Midnight"), today(24)),
		  command(q_("Sunset"), sun(2)),
		  command(q_("Sunrise"), sun(0)),
		  command(q_("Dusk"), [] { trigger("actionToday_EveningTwilight"); }),
		  command(q_("Dawn"), [] { trigger("actionToday_MorningTwilight"); }),
		  branch(q_("Set date"), "bbtTime",
			 [] {
				 return QList<Section>{
					 {q_("Date"), {dateStepper(N_("Year"), &LocalDate::year), dateStepper(N_("Month"), &LocalDate::month),
						       dateStepper(N_("Day"), &LocalDate::day)}},
					 {q_("Time"), {dateStepper(N_("Hour"), &LocalDate::hour), dateStepper(N_("Minute"), &LocalDate::minute),
						       command(q_("Now"), [] { trigger("actionReturn_To_Current_Time"); }, "btTimeNow")}}};
			 },
			 date)}},
	};
}

QList<Section> OpenXRMenu::locationContent()
{
	StelCore* core = StelApp::getInstance().getCore();
	const auto moveTo = [core](const StelLocation& location) {
		return command(location.name, [core, location] { core->moveObserverTo(location, 0., 0.); }, QString(),
			       [core, location] { const StelLocation& here = core->getCurrentLocation();
					          return here.name == location.name && here.planetName == location.planetName; });
	};
	Section dark{q_("Dark skies")};
	Item homeItem = moveTo(home);
	homeItem.label = q_("Home");
	homeItem.icon = "bbtLocation";
	dark.items << homeItem;
	for (const Place& place : places)
	{
		Item item = moveTo(placeLocation(place));
		item.label = q_(place.name);
		dark.items << item;
	}

	// Continents, their regions, and each region's biggest towns.
	const auto browse = [moveTo] {
		QMap<QString, QStringList> continents;
		for (const QString& region : StelApp::getInstance().getLocationMgr().getRegionNames("Earth"))
			continents[continentOf(region)] << region;
		Section section{q_("Continents")};
		for (auto it = continents.cbegin(); it != continents.cend(); ++it)
		{
			const QStringList regions = it.value();
			section.items << branch(q_(it.key()), QString(), [regions, moveTo] {
				Section inner{q_("Regions")};
				for (const QString& region : regions)
					inner.items << branch(q_(region), QString(), [region, moveTo] {
						QList<StelLocation> towns = StelApp::getInstance().getLocationMgr().pickLocationsInRegion(region).values();
						std::sort(towns.begin(), towns.end(), [](const StelLocation& a, const StelLocation& b) {
							return a.population > b.population;
						});
						Section cities{q_("Biggest towns")};
						for (const StelLocation& town : towns.mid(0, 120))
							cities.items << moveTo(town);
						return QList<Section>{cities};
					});
				return QList<Section>{inner};
			});
		}
		return QList<Section>{section};
	};

	const auto worldsList = [moveTo] {
		Section section{q_("Other worlds")};
		for (const char* world : worlds)
		{
			Item item = moveTo(StelLocation(q_(world), QString(), QString(), world, 0.f, 0.f, 0, 0, "LMST", 1, 'X'));
			item.label = q_(world);
			section.items << item;
		}
		return QList<Section>{section};
	};

	return {dark,
		{q_("More places"),
		 {branch(q_("Browse the world"), "bbtLocation", browse),
		  branch(q_("Other worlds"), "btPlanets", worldsList, [core] { return q_(core->getCurrentLocation().planetName); })}}};
}

QList<Section> OpenXRMenu::findContent()
{
	const auto list = [this](const QString& title, const QStringList& names, bool translate) {
		return [this, title, names, translate] {
			Section section{title};
			for (const QString& name : names)
				section.items << findItem(translate ? q_(name) : name, name, showMe);
			return QList<Section>{section};
		};
	};
	QStringList planetNames, starNames, messier;
	for (const char* name : planets)
		planetNames << name;
	for (const char* name : stars)
		starNames << name;
	for (int i = 1; i <= 110; ++i)
		messier << QString("M%1").arg(i);
	QStringList constellations = GETSTELMODULE(ConstellationMgr)->getConstellationsEnglishNames();
	constellations.sort();
	return {{q_("Lists"),
		 {branch(q_("Planets"), "btPlanets", list(q_("Planets"), planetNames, true)),
		  branch(q_("Bright stars"), "bbtSky", list(q_("Bright stars"), starNames, false)),
		  branch(q_("Constellations"), "btConstellationLines", list(q_("Constellations"), constellations, false)),
		  branch(q_("Messier"), "btNebula", list(q_("Messier"), messier, false))}}};
}

QList<Section> OpenXRMenu::viewContent()
{
	return {
		{q_("Menu"),
		 {stepper(q_("Size"), [this] { return QString("%1%").arg(qRound(scale * 100)); },
			  [this](int by) { scale = qBound(0.7, scale + 0.1 * by, 1.5); }),
		  command(q_("Recenter"), [this] { if (recenter) recenter(); }, "btCompass"),
		  command(q_("Full GUI"), [this] { if (openGui) openGui(); }, "bbtSettings", [this] { return guiOpen && guiOpen(); })}},
	};
}

QStringList OpenXRMenu::cardLines() const
{
	const QList<StelObjectP> selected = GETSTELMODULE(StelObjectMgr)->getSelectedObject();
	if (selected.isEmpty())
		return {};
	const StelObjectP object = selected.first();
	StelCore* core = StelApp::getInstance().getCore();

	QString name = object->getNameI18n();
	if (name.isEmpty())
		name = object->getID();
	if (name.isEmpty()) // some dark nebulae, for one
		name = q_("Unnamed %1").arg(object->getObjectTypeI18n());
	QStringList lines = {name, QString("%1   %2 %3").arg(object->getObjectTypeI18n(), q_("magnitude"))
					     .arg(double(object->getVMagnitude(core)), 0, 'f', 1)};

	double az = 0., alt = 0.;
	StelUtils::rectToSphe(&az, &alt, object->getAltAzPosApparent(core));
	az = 3. * M_PI - az; // from the south, counterclockwise, to from the north, clockwise
	if (az > 2. * M_PI)
		az -= 2. * M_PI;
	lines << QString::fromUtf8("%1 %2°   %3 %4°").arg(q_("Altitude")).arg(alt * M_180_PI, 0, 'f', 1)
			 .arg(q_("Azimuth")).arg(az * M_180_PI, 0, 'f', 1);

	const Vec4d rts = object->getRTSTime(core);
	if (rts[3] == 100.)
		lines << q_("Circumpolar: never sets");
	else if (rts[3] == -100.)
		lines << q_("Never rises");
	else if (rts[3] > -1000.)
		lines << QString("%1 %2   %3 %4   %5 %6").arg(q_("Rises"), localTime(rts[0]), q_("Transits"), localTime(rts[1]),
								   q_("Sets"), localTime(rts[2]));
	else
		lines << QString();
	return lines;
}


void OpenXRMenu::layoutHand(Canvas& canvas, int hoverTile)
{
	// The title: the category, the ones on either side a click or a flick of
	// the stick away, and where it is among them.
	const QRect previous(margin, margin, titleHeight, titleHeight), next(handWidth - margin - titleHeight, margin, titleHeight, titleHeight);
	canvas.target(previous, [this] { turnCategory(-1); }, false, hoverTile);
	canvas.text(previous, QString::fromUtf8("‹"), Qt::AlignCenter, 44);
	canvas.target(next, [this] { turnCategory(1); }, false, hoverTile);
	canvas.text(next, QString::fromUtf8("›"), Qt::AlignCenter, 44);
	const QRect title(previous.right() + gap, margin, next.left() - previous.right() - 2 * gap, titleHeight - 16);
	const QString name = q_(categories[category].label);
	const int nameWidth = qMin(QFontMetrics([] { QFont f = QGuiApplication::font(); f.setPixelSize(fontPixels); f.setBold(true); return f; }())
					   .horizontalAdvance(name), title.width() - iconSize - 12);
	const int left = title.center().x() - (iconSize + 12 + nameWidth) / 2;
	canvas.icon(QRect(left, title.center().y() - iconSize / 2, iconSize, iconSize), categories[category].icon);
	canvas.text(QRect(left + iconSize + 12, title.top(), nameWidth + 4, title.height()), name, Qt::AlignLeft | Qt::AlignVCenter,
		    fontPixels, Qt::white, true);
	const int dotsLeft = title.center().x() - (categories.size() * 20 - 8) / 2;
	for (int i = 0; i < categories.size(); ++i)
		canvas.box(QRect(dotsLeft + i * 20, margin + titleHeight - 14, 12, 12), i == category ? QColor(Qt::white) : dimText.darker(200), false);

	// Back, and the levels opened below the category.
	if (!path.isEmpty())
	{
		const QRect back(margin, crumbTop, 150, crumbHeight);
		canvas.target(back, [this] { if (!path.isEmpty()) path.removeLast(); }, false, hoverTile);
		canvas.text(back, QString::fromUtf8("‹ ") + q_("Back"), Qt::AlignCenter);
		QStringList trail;
		for (const Level& level : path)
			trail << level.title;
		canvas.text(QRect(back.right() + gap + 8, crumbTop, handWidth - margin - back.right() - gap - 8, crumbHeight),
			    trail.join(QString::fromUtf8(" › ")), Qt::AlignLeft | Qt::AlignVCenter, fontPixels, Qt::white, true);
	}

	// Sections of tiles, cut into pages that fit.
	const QList<Section> sections = path.isEmpty() ? categories[category].content() : path.last().content();
	struct Row
	{
		QString heading;
		QList<Item> items;
	};
	QList<QList<Row>> pages(1);
	int used = 0;
	for (const Section& section : sections)
	{
		// Branches with an icon or showing what they are set to take two columns.
		QList<QList<Item>> rows;
		int filled = columns;
		for (const Item& item : section.items)
		{
			const int span = wide(item) ? 2 : 1;
			if (filled + span > columns)
			{
				rows.append(QList<Item>());
				filled = 0;
			}
			rows.last() << item;
			filled += span;
		}
		for (int index = 0; index < rows.size(); ++index)
		{
			// A heading goes with the first row under it.
			const bool first = index == 0;
			const int need = (first ? headingHeight : 0) + tileHeight;
			if (used + need > contentHeight && used > 0)
			{
				pages.append(QList<Row>());
				used = 0;
			}
			if (first || used == 0)
			{
				pages.last().append(Row{first ? section.title : section.title + QString::fromUtf8(" …"), {}});
				used += headingHeight;
			}
			pages.last().append(Row{QString(), rows[index]});
			used += tileHeight + gap;
		}
	}
	int& page = path.isEmpty() ? categoryPage : path.last().page;
	page = qBound(0, page, int(pages.size()) - 1);

	int y = contentTop;
	for (const Row& row : pages[page])
	{
		if (row.items.isEmpty())
		{
			canvas.text(QRect(margin + 4, y, handWidth - 2 * margin, headingHeight), row.heading, Qt::AlignLeft | Qt::AlignVCenter,
				    smallPixels, dimText, true);
			y += headingHeight;
			continue;
		}
		int column = 0;
		for (const Item& item : row.items)
		{
			const int span = wide(item) ? 2 : 1;
			const QRect r(margin + column * (tileWidth + gap), y, span * tileWidth + (span - 1) * gap, tileHeight);
			column += span;
			if (item.step)
			{
				// A stepper: its label on top, and a - and a + half under it.
				const QRect minus(r.left(), r.top(), r.width() / 2, r.height()), plus(minus.right() + 1, r.top(), r.width() - minus.width(), r.height());
				canvas.target(minus, [step = item.step] { step(-1); }, false, hoverTile);
				canvas.target(plus, [step = item.step] { step(1); }, false, hoverTile);
				canvas.text(QRect(r.left() + 8, r.top() + 4, r.width() - 16, 36), item.label, Qt::AlignCenter, smallPixels, dimText);
				canvas.text(QRect(r.left() + 44, r.top() + 40, r.width() - 88, 56), item.value(), Qt::AlignCenter, fontPixels, Qt::white, true);
				canvas.text(QRect(r.left() + 6, r.top() + 40, 40, 56), QString::fromUtf8("−"), Qt::AlignCenter, 40);
				canvas.text(QRect(r.right() - 45, r.top() + 40, 40, 56), "+", Qt::AlignCenter, 40);
				continue;
			}
			std::function<void()> act = item.run;
			if (item.open)
			{
				const Level level{item.label, item.open};
				act = [this, level] { path.append(level); };
			}
			canvas.target(r, act, item.on && item.on(), hoverTile);
			QRect text = r.adjusted(10, 6, -10, -6);
			if (!item.icon.isEmpty())
			{
				canvas.icon(QRect(r.left() + 10, r.center().y() - iconSize / 2, iconSize, iconSize), item.icon);
				text.setLeft(r.left() + 10 + iconSize + 8);
			}
			if (item.open)
			{
				canvas.text(QRect(r.right() - 34, r.top(), 30, r.height()), QString::fromUtf8("›"), Qt::AlignCenter, fontPixels, dimText);
				text.setRight(r.right() - 34);
			}
			if (item.value)
			{
				canvas.text(text.adjusted(0, 0, 0, -text.height() / 2), item.label, Qt::AlignLeft | Qt::AlignVCenter, smallPixels);
				canvas.text(text.adjusted(0, text.height() / 2, 0, 0), item.value(), Qt::AlignLeft | Qt::AlignVCenter, smallPixels, dimText);
			}
			else
				canvas.text(text, item.label, (item.icon.isEmpty() && !item.open ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter | Qt::TextWordWrap, smallPixels);
		}
		y += tileHeight + gap;
	}

	if (pages.size() > 1)
	{
		const QRect back(margin, pagerTop, 200, pagerHeight), forward(handWidth - margin - 200, pagerTop, 200, pagerHeight);
		if (page > 0)
		{
			canvas.target(back, [this] { turnPage(-1); }, false, hoverTile);
			canvas.text(back, QString::fromUtf8("‹ ") + q_("Previous"), Qt::AlignCenter);
		}
		canvas.text(QRect(back.right(), pagerTop, forward.left() - back.right(), pagerHeight),
			    QString("%1 / %2").arg(page + 1).arg(pages.size()), Qt::AlignCenter, smallPixels, dimText);
		if (page < pages.size() - 1)
		{
			canvas.target(forward, [this] { turnPage(1); }, false, hoverTile);
			canvas.text(forward, q_("Next") + QString::fromUtf8(" ›"), Qt::AlignCenter);
		}
	}
}

bool OpenXRMenu::canGoTo()
{
	// What StelCore::moveObserverToSelected() lands on: a solar system body
	// other than the one here, or a feature on one, as a crater.
	const QList<StelObjectP> selected = GETSTELMODULE(StelObjectMgr)->getSelectedObject();
	if (selected.isEmpty())
		return false;
	if (dynamic_cast<const NomenclatureItem*>(selected.first().data()))
		return true;
	const Planet* body = dynamic_cast<const Planet*>(selected.first().data());
	return body && body->getEnglishName() != StelApp::getInstance().getCore()->getCurrentLocation().planetName;
}

void OpenXRMenu::layoutCard(Canvas& canvas, int hoverTile) const
{
	const QStringList lines = cardLines();
	if (lines.isEmpty())
		return;
	canvas.text(QRect(margin + 4, margin, cardWidth - 2 * margin, 44), lines[0], Qt::AlignLeft | Qt::AlignVCenter, fontPixels,
		    Qt::white, true);
	QRect line(margin + 4, margin + 44, cardWidth - 2 * margin, cardLineHeight);
	for (int i = 1; i < lines.size(); ++i, line.translate(0, cardLineHeight))
		canvas.text(line, lines[i], Qt::AlignLeft | Qt::AlignVCenter, smallPixels, dimText);

	QList<QPair<QString, std::function<void()>>> buttons = {
		{q_("Rise"), [] { trigger("actionNext_Rising"); }},
		{q_("Transit"), [] { trigger("actionNext_Transit"); }},
		{q_("Set"), [] { trigger("actionNext_Setting"); }},
		{q_("Deselect"), [] { GETSTELMODULE(StelObjectMgr)->unSelect(); }},
	};
	if (canGoTo())
		buttons.insert(3, {q_("Go there"), [] {
			// Stellarium's own landing: on the body, or on the feature's.
			StelApp::getInstance().getCore()->moveObserverToSelected();
			GETSTELMODULE(StelObjectMgr)->unSelect(); // it is underfoot now
		}});
	const int buttonWidth = (cardWidth - 2 * margin - (buttons.size() - 1) * gap) / buttons.size();
	for (int i = 0; i < buttons.size(); ++i)
	{
		const QRect r(margin + i * (buttonWidth + gap), cardHeight - margin - cardButtonHeight, buttonWidth, cardButtonHeight);
		canvas.target(r, buttons[i].second, false, hoverTile);
		// Five to a row are narrow: labels shrink, or wrap, to fit.
		canvas.text(r.adjusted(4, 2, -4, -2), buttons[i].first, Qt::AlignCenter | Qt::TextWordWrap, smallPixels);
	}
}

QImage OpenXRMenu::render(Surface surface, int hoverTile)
{
	Canvas canvas;
	if (surface == Hand)
		layoutHand(canvas, hoverTile);
	else
		layoutCard(canvas, hoverTile);
	targets[surface] = canvas.targets;

	const QString state = canvas.state();
	if (state == drawnState[surface])
		return QImage();
	drawnState[surface] = state;

	QImage image(size(surface), QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(panelColor);
	painter.drawRoundedRect(image.rect(), 24, 24);
	canvas.draw(painter);
	painter.end();
	return image;
}

QImage OpenXRMenu::renderTimeBar()
{
	StelCore* core = StelApp::getInstance().getCore();
	const StelLocaleMgr& locale = StelApp::getInstance().getLocaleMgr();
	const double jd = core->getJD(), utcOffset = core->getUTCOffset(jd);
	const double rate = core->getTimeRate() / StelCore::JD_SECOND;
	const QString date = locale.getPrintableDateLocal(jd, utcOffset), time = locale.getPrintableTimeLocal(jd, utcOffset);
	const QString speed = rate == 0. ? q_("paused") : QString::fromUtf8("×%1").arg(rate, 0, 'g', 4);
	const QString state = date + time + speed;
	if (state == timeBarState)
		return QImage();
	timeBarState = state;

	QImage image(timeBarWidth, timeBarHeight, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(panelColor);
	painter.drawRoundedRect(image.rect(), timeBarHeight / 2, timeBarHeight / 2);
	QFont font = QGuiApplication::font();
	font.setPixelSize(fontPixels);
	const QRect inside = image.rect().adjusted(32, 0, -32, 0);
	painter.setFont(font);
	painter.setPen(dimText);
	painter.drawText(inside, Qt::AlignLeft | Qt::AlignVCenter, date);
	painter.drawText(inside, Qt::AlignRight | Qt::AlignVCenter, speed);
	font.setBold(true);
	font.setPixelSize(36);
	painter.setFont(font);
	painter.setPen(Qt::white);
	painter.drawText(inside, Qt::AlignCenter, time);
	painter.end();
	return image;
}
