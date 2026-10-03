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

#ifndef OPENXRMENU_HPP
#define OPENXRMENU_HPP

#include "StelLocation.hpp"

#include <QImage>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <functional>

//! What the VR controls show, each drawn into an image for the headset:
//! - the hand menu, over the left hand: a category at a time
//!   (sky, constellations, lines, landscape, time, location, finding things,
//!   the view) of grouped options, some opening deeper levels, like the list of
//!   sky cultures, shown in its place under a Back button. Nothing needs a
//!   keyboard;
//! - the card of the object picked, which floats in the sky beside it;
//! - the time bar, shown while the time changes.
//! OpenXR places them and does the pointing.
class OpenXRMenu
{
public:
	OpenXRMenu();

	//! What the menu leaves to the headset.
	std::function<void()> recenter, openGui, showMe;
	std::function<bool()> guiOpen;

	//! How big the hand menu is, set from the menu itself.
	double scale = 1.;

	//! What can be pointed at and clicked.
	enum Surface { Hand, Card, Surfaces };
	static QSize size(Surface surface);
	//! What a click at this pixel of the surface would hit, or -1.
	int tileAt(Surface surface, const QPointF& pixel) const;
	//! What clicking it does, taken when it is clicked: the surface may be laid
	//! out again before it runs.
	std::function<void()> actionFor(Surface surface, int tile) const;
	//! Draws the surfaces again at the next render(), after a click.
	void changed();
	//! The surface as it looks now, or a null image when nothing on it changed
	//! since the last call.
	QImage render(Surface surface, int hoverTile);
	//! Whether there is an object picked, for a card.
	static bool hasCard();
	//! Whether the observer can go to the object picked, as to a planet.
	static bool canGoTo();
	//! Moves the hand menu to another category, by the stick.
	void turnCategory(int by);
	//! And to another page of it, kept to those there are when laid out.
	void turnPage(int by) { (path.isEmpty() ? categoryPage : path.last().page) += by; }

	//! The date, time and rate, or a null image when unchanged.
	QImage renderTimeBar();

	//! Where the Home tile goes back to.
	void setHome(const StelLocation& location) { home = location; }

	struct Section;
	//! One tile: a toggle (on and run), a command (run), a stepper (value and
	//! step, with a - and a + half) or a branch (open, a level deeper).
	struct Item
	{
		QString label, icon;
		std::function<bool()> on;
		std::function<void()> run;
		std::function<QString()> value;
		std::function<void(int)> step;
		std::function<QList<Section>()> open;
	};
	struct Section
	{
		QString title;
		QList<Item> items;
	};

private:
	struct Level
	{
		QString title;
		std::function<QList<Section>()> content;
		int page = 0;
	};
	struct Category
	{
		QString label, icon;
		std::function<QList<Section>()> content;
	};
	//! Where a click lands, set up with each drawing of the menu.
	struct Target
	{
		QRect rect;
		std::function<void()> act;
	};

	QStringList cardLines() const;
	QList<Section> skyContent();
	QList<Section> constellationContent();
	QList<Section> linesContent();
	QList<Section> landscapeContent();
	QList<Section> timeContent();
	QList<Section> locationContent();
	QList<Section> findContent();
	QList<Section> viewContent();
	static Item branch(const QString& label, const QString& icon, std::function<QList<Section>()> open,
		    std::function<QString()> value = nullptr);
	class Canvas;
	void layoutHand(Canvas& canvas, int hoverTile);
	void layoutCard(Canvas& canvas, int hoverTile) const;

	QList<Category> categories;
	int category = 0, categoryPage = 0;
	//! The levels opened below the category, deepest last.
	QList<Level> path;
	QList<Target> targets[Surfaces];
	QString drawnState[Surfaces];
	QString timeBarState;
	StelLocation home;
};

#endif // OPENXRMENU_HPP
