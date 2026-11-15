// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamecards.h"

#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "games.h"
#include "theme.h"

namespace {

constexpr int kRow = Qt::UserRole;          // a card's row in GameCards::m_shown
constexpr int kSeries = Qt::UserRole + 1;   // a shelf's series ("" all)
constexpr int kColour = Qt::UserRole + 2;   // ... its colour (none for all)
constexpr int kCount = Qt::UserRole + 3;    // ... its games, as said beside it

// a card: the picture 4:3, its title, a line under it, a status
constexpr int kPicW = 184, kPicH = 138, kPad = 6, kGap = 12;

int radius() { return theme::pop() ? 12 : 4; }

QColor between(QColor a, QColor b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t, a.alphaF() * (1 - t) + b.alphaF() * t);
}

// a series' colour where it is seen against `ground`: one too close to it (IIDX's near-black on a
// dark list) made lighter or darker until it shows
QColor against(QColor c, QColor ground) {
    const bool dark = ground.lightnessF() < 0.5;
    for (int k = 0; k < 6 && std::abs(c.lightnessF() - ground.lightnessF()) < 0.22; k++) c = dark ? c.lighter(150) : c.darker(130);
    return c;
}

// a title screen at a card's size, cropped to fill it; kept while its file stays the same
QPixmap picture(const QString &path, QSize size, qreal dpr) {
    static QHash<QString, QPixmap> kept;
    const QFileInfo f(path);
    const QString key = path + "|" + QString::number(f.lastModified().toMSecsSinceEpoch()) + "|" +
                        QString::number(size.width()) + "x" + QString::number(size.height()) + "@" + QString::number(dpr);
    if (auto it = kept.constFind(key); it != kept.constEnd()) return *it;
    QImageReader r(path);
    QImage img = r.read();
    QPixmap pm;
    if (!img.isNull()) {
        const QSize px = size * dpr;
        img = img.scaled(px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        img = img.copy((img.width() - px.width()) / 2, (img.height() - px.height()) / 2, px.width(), px.height());
        pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(dpr);
    }
    if (kept.size() > 400) kept.clear();
    kept.insert(key, pm);
    return pm;
}

// A game's picture in `pic` (rounded), or a stand-in in its series' colour: its badge and a word on
// when there'll be one. `big`: the chosen game's (GameHead), its badge and words larger
void paintPicture(QPainter *p, const QRectF &pic, const Card &c, const QFont &font, const QPalette &pal, bool big) {
    QPainterPath clip;
    clip.addRoundedRect(pic, radius() + (big ? 4 : 0), radius() + (big ? 4 : 0));
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setClipPath(clip);
    if (!c.ready) p->setOpacity(0.45);
    const qreal dpr = p->device()->devicePixelRatioF();
    const QPixmap pm = c.picture.isEmpty() ? QPixmap() : picture(c.picture, pic.size().toSize(), dpr);
    if (!pm.isNull()) {
        p->drawPixmap(pic.topLeft(), pm);
    } else {
        const QColor col = against(familyColour(c.family), pal.color(QPalette::Base));
        QLinearGradient g(pic.topLeft(), pic.bottomRight());
        g.setColorAt(0, col);
        g.setColorAt(1, col.darker(170));
        p->fillRect(pic, g);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(255, 255, 255, 34));
        p->drawEllipse(QPointF(pic.right() - pic.width() * 0.12, pic.top() + pic.height() * 0.1), pic.width() * 0.45, pic.width() * 0.45);
        const int b = big ? 84 : 52;
        p->drawPixmap(QRectF(pic.center().x() - b / 2.0, pic.center().y() - b / 2.0 - (big ? 14 : 10), b, b),
                      familyIcon(c.family).pixmap(QSize(b, b), dpr), QRectF());
        QFont f = font;
        f.setPointSizeF(f.pointSizeF() * (big ? 0.95 : 0.8));
        p->setFont(f);
        p->setPen(QColor(255, 255, 255, 200));
        p->drawText(pic.adjusted(4, 0, -4, big ? -12 : -8), Qt::AlignHCenter | Qt::AlignBottom,
                    !c.ready ? "not added yet" : c.keepsPicture ? "its title screen, at its next start" : "right-click to choose a picture");
    }
    p->restore();
}

class CardPainter : public QStyledItemDelegate {
public:
    CardPainter(GameCards *cards) : QStyledItemDelegate(cards), m_cards(cards) {}
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &) const override {
        const QFontMetrics fm(o.font);
        return QSize(kPicW + 2 * kPad, kPad + kPicH + 6 + fm.height() + fm.height() + 4 + fm.height() + 6 + kPad);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        const Card *c = m_cards->card(i.data(kRow).toInt());
        if (!c) return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = o.palette;
        const bool sel = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
        const QRectF box = QRectF(o.rect).adjusted(2, 2, -2, -2);
        const QColor accent = pal.color(QPalette::Highlight);
        if (sel || hover) {
            p->setPen(sel ? QPen(accent, 2.5) : Qt::NoPen);
            p->setBrush(sel ? between(pal.color(QPalette::Base), accent, 0.12) : between(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.06));
            p->drawRoundedRect(box.adjusted(1, 1, -1, -1), radius() + 2, radius() + 2);
        }
        const QRectF pic(box.left() + kPad - 2, box.top() + kPad - 2, kPicW, kPicH);
        paintPicture(p, pic, *c, o.font, pal, false);
        // the title, the line under it, the status
        const QFontMetrics fm(o.font);
        QFont bold = o.font;
        bold.setBold(true);
        const QFontMetrics bm(bold);
        double y = pic.bottom() + 6;
        const QColor ink = c->ready ? pal.color(QPalette::Text) : pal.color(QPalette::Disabled, QPalette::Text);
        const QColor quiet = pal.color(QPalette::PlaceholderText);
        p->setFont(bold);
        p->setPen(ink);
        p->drawText(QRectF(pic.left() + 2, y, pic.width() - 4, bm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    bm.elidedText(c->title, Qt::ElideRight, int(pic.width() - 4)));
        y += bm.height();
        p->setFont(o.font);
        p->setPen(quiet);
        p->drawText(QRectF(pic.left() + 2, y, pic.width() - 4, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(c->detail, Qt::ElideRight, int(pic.width() - 4)));
        y += fm.height() + 4;
        if (!c->status.isEmpty()) {
            QFont small = bold;
            small.setPointSizeF(small.pointSizeF() * 0.85);
            const QFontMetrics sm(small);
            const QColor col = !c->ready ? quiet : c->notable ? theme::notable() : theme::ready();
            const QString t = sm.elidedText(c->status, Qt::ElideRight, int(pic.width() - 16));
            const QRectF pill(pic.left(), y, sm.horizontalAdvance(t) + 14, sm.height() + 4);
            p->setPen(Qt::NoPen);
            QColor bg = col;
            bg.setAlphaF(0.16);
            p->setBrush(bg);
            const double r = theme::pop() ? pill.height() / 2 : 3;
            p->drawRoundedRect(pill, r, r);
            p->setFont(small);
            p->setPen(col);
            p->drawText(pill, Qt::AlignCenter, t);
        }
        p->restore();
    }
private:
    GameCards *m_cards;
};

// a shelf: its colour's dot (the six for all the games), its name, its count
class ShelfPainter : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        QSize s = QStyledItemDelegate::sizeHint(o, i);
        s.setHeight(std::max(s.height(), QFontMetrics(o.font).height() + 14));
        return s;
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = o.palette;
        const bool sel = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
        const QRectF r = QRectF(o.rect).adjusted(4, 1, -4, -1);
        const QColor accent = pal.color(QPalette::Highlight);
        if (sel || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(sel ? between(pal.color(QPalette::Base), accent, theme::pop() ? 0.2 : 0.3)
                            : between(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.06));
            p->drawRoundedRect(r, radius(), radius());
            if (sel) {
                p->setBrush(accent);
                p->drawRoundedRect(QRectF(r.left(), r.top() + 6, 3.5, r.height() - 12), 1.75, 1.75);
            }
        }
        const double d = 9, cy = r.center().y();
        p->setPen(Qt::NoPen);
        const QColor col = against(i.data(kColour).value<QColor>(), pal.color(QPalette::Base));
        if (i.data(kColour).isValid()) {
            p->setBrush(col);
            p->drawEllipse(QPointF(r.left() + 16, cy), d / 2, d / 2);
        } else
            for (int k = 0; k < 6; k++) p->fillRect(QRectF(r.left() + 10 + k * 2.2, cy - 3, 2.3, 6), theme::rainbow(k));
        QFont f = o.font;
        f.setBold(sel);
        p->setFont(f);
        const QFontMetrics fm(f);
        const QString count = i.data(kCount).toString();
        const double cw = QFontMetrics(o.font).horizontalAdvance(count) + 8;
        p->setPen(pal.color(QPalette::Text));
        p->drawText(QRectF(r.left() + 30, r.top(), r.width() - 30 - cw, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(r.width() - 30 - cw)));
        p->setFont(o.font);
        p->setPen(pal.color(QPalette::PlaceholderText));
        p->drawText(QRectF(r.right() - cw, r.top(), cw - 6, r.height()), Qt::AlignRight | Qt::AlignVCenter, count);
        p->restore();
    }
};

// The chosen game's picture, title, line and chips, laid out for a width (painted, or only measured)
class HeadArt : public QWidget {
public:
    const Head *h = nullptr;
    // its height for its width, set as its own (a scroll area's layout doesn't ask again when only
    // the chips change: the Play under them covered them)
    void fit() {
        const int hh = place(width() > 0 ? width() : 300, nullptr);
        if (height() != hh || minimumHeight() != hh) setFixedHeight(hh);
        update();
    }
protected:
    void resizeEvent(QResizeEvent *) override { fit(); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        place(width(), &p);
    }
private:
    int place(int w, QPainter *p) const {
        if (!h || w <= 0) return 0;
        const QPalette pal = palette();
        const QRectF pic(0, 0, w, std::round(w * 0.75));
        if (p) paintPicture(p, pic, h->card, font(), pal, true);
        double y = pic.bottom() + 12;
        auto text = [&](const QFont &f, const QColor &c, const QString &t, double after) {
            const QRect r = QFontMetrics(f).boundingRect(QRect(0, 0, w, 100000), Qt::TextWordWrap, t);
            if (p) {
                p->setFont(f);
                p->setPen(c);
                p->drawText(QRectF(0, y, w, r.height()), Qt::TextWordWrap, t);
            }
            y += r.height() + after;
        };
        QFont title = font();
        title.setPointSizeF(title.pointSizeF() * 1.45);
        title.setBold(true);
        text(title, h->card.ready ? pal.color(QPalette::Text) : pal.color(QPalette::Disabled, QPalette::Text), h->card.title, 2);
        text(font(), pal.color(QPalette::PlaceholderText), h->line, 10);
        // the chips: what its next start does (when special), then the mods on, each its colour
        struct Chip { QString t; QColor c; };
        QList<Chip> chips;
        if (h->card.notable && !h->card.status.isEmpty()) chips << Chip{h->card.status, theme::notable()};
        for (int k = 0; k < h->mods.size(); k++)
            chips << Chip{h->mods[k], theme::pop() ? theme::rainbow((k * 5) % 6) : pal.color(QPalette::Highlight)};
        if (!chips.isEmpty()) {
            QFont f = font();
            f.setBold(true);
            f.setPointSizeF(f.pointSizeF() * 0.9);
            const QFontMetrics fm(f);
            const double ch = fm.height() + 8, gap = 6;
            double x = 0;
            for (const Chip &c : chips) {
                const QString t = fm.elidedText(c.t, Qt::ElideRight, w - 20);
                const double cw = fm.horizontalAdvance(t) + 20;
                if (x > 0 && x + cw > w) x = 0, y += ch + gap;
                if (p) {
                    const QColor ink = against(c.c, pal.color(QPalette::Base));
                    QColor bg = ink;
                    bg.setAlphaF(0.17);
                    p->setPen(Qt::NoPen);
                    p->setBrush(bg);
                    const double r = theme::pop() ? ch / 2 : 3;
                    p->drawRoundedRect(QRectF(x, y, cw, ch), r, r);
                    p->setFont(f);
                    p->setPen(ink);
                    p->drawText(QRectF(x, y, cw, ch), Qt::AlignCenter, t);
                }
                x += cw + gap;
            }
            y += ch + 4;
        }
        return int(std::ceil(y));
    }
};

}  // namespace

GameCards::GameCards(QWidget *parent) : QWidget(parent) {
    m_shelf = cfg::settings().value("ui/shelf").toString();
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_shelves = new QListWidget;
    m_shelves->setItemDelegate(new ShelfPainter(m_shelves));
    m_shelves->setMouseTracking(true);
    m_shelves->setFrameShape(QFrame::NoFrame);
    m_shelves->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_shelves->setFixedWidth(220);
    m_shelves->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(m_shelves);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(16, 12, 4, 0);
    rv->setSpacing(2);
    m_head = new QLabel;
    QFont big = m_head->font();
    big.setPointSizeF(big.pointSizeF() * 1.7);
    big.setBold(true);
    m_head->setFont(big);
    rv->addWidget(m_head);
    m_sub = new QLabel;
    rv->addWidget(m_sub);
    rv->addSpacing(6);
    m_grid = new QListWidget;
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setWrapping(true);
    m_grid->setSpacing(0);   // (the gap is in the grid's cells: fitGrid)
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setMouseTracking(true);
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setItemDelegate(new CardPainter(this));
    m_grid->viewport()->installEventFilter(this);   // (its width: fitGrid)
    rv->addWidget(m_grid, 1);
    lay->addWidget(right, 1);

    connect(m_shelves, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (m_quiet || !it) return;
        m_shelf = it->data(kSeries).toString();
        cfg::settings().setValue("ui/shelf", m_shelf);
        showShelf(true);
    });
    connect(m_grid, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (m_quiet || !it) return;
        if (const Card *c = card(it->data(kRow).toInt())) {
            m_current = c->id;
            emit picked(c->id);
        }
    });
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        if (const Card *c = card(it->data(kRow).toInt())) emit activated(c->id);
    });
    connect(m_grid, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *it = m_grid->itemAt(pos);
        const Card *c = it ? card(it->data(kRow).toInt()) : nullptr;
        if (!c) return;
        m_grid->setCurrentItem(it);
        emit menuAsked(c->id, m_grid->viewport()->mapToGlobal(pos));
    });
    themeChanged();
}

const Card *GameCards::card(int row) const {
    return row >= 0 && row < m_shown.size() ? &m_all[m_shown[row]] : nullptr;
}

void GameCards::setCards(const QList<Card> &cards) {
    m_all = cards;
    bool there = m_shelf.isEmpty();
    for (const Card &c : m_all) there = there || c.series == m_shelf;
    if (!there) m_shelf.clear();   // (its games all hidden by a search: all of them, not an empty shelf)
    fillShelves();
    showShelf(false);
}

void GameCards::fillShelves() {
    m_quiet = true;
    m_shelves->clear();
    struct Count { int n = 0, ready = 0; QHash<QString, int> families; };
    QStringList order;
    QHash<QString, Count> counts;
    Count all;
    for (const Card &c : m_all) {
        if (!counts.contains(c.series)) order << c.series;
        Count &k = counts[c.series];
        k.families[c.family]++;
        k.n++, all.n++;
        k.ready += c.ready, all.ready += c.ready;
    }
    auto add = [this](const QString &name, const QString &series, QColor colour, int n) {
        auto *it = new QListWidgetItem(name, m_shelves);
        it->setData(kSeries, series);
        if (colour.isValid()) it->setData(kColour, colour);
        it->setData(kCount, QString::number(n));
        it->setToolTip(name);
        if (series == m_shelf) m_shelves->setCurrentItem(it);
    };
    add("All games", QString(), QColor(), all.n);
    // a series' colour: its family with the most games (pop'n music's pink, not its first game's yellow)
    for (const QString &s : order) {
        QString most;
        for (auto it = counts[s].families.cbegin(); it != counts[s].families.cend(); ++it)
            if (most.isEmpty() || it.value() > counts[s].families[most] || (it.value() == counts[s].families[most] && it.key() < most)) most = it.key();
        add(s, s, familyColour(most), counts[s].n);
    }
    m_quiet = false;
}

void GameCards::showShelf(bool pickFirst) {
    m_quiet = true;
    m_grid->clear();
    m_shown.clear();
    int ready = 0, at = -1;
    for (int k = 0; k < m_all.size(); k++) {
        const Card &c = m_all[k];
        if (!m_shelf.isEmpty() && c.series != m_shelf) continue;
        auto *it = new QListWidgetItem(m_grid);
        it->setData(kRow, (int)m_shown.size());
        it->setToolTip(c.title + (c.status.isEmpty() ? QString() : "\n" + c.status));
        if (c.id == m_current) at = m_shown.size();
        m_shown << k;
        ready += c.ready;
    }
    m_head->setText(m_shelf.isEmpty() ? QString("All games") : m_shelf);
    const int n = m_shown.size();
    m_sub->setText(n == ready ? QString("%1 game%2").arg(n).arg(n == 1 ? "" : "s")
                              : QString("%1 games · %2 ready to play").arg(n).arg(ready));
    if (at < 0 && pickFirst && n) at = 0;
    if (at >= 0) {
        m_grid->setCurrentRow(at);
        m_grid->scrollToItem(m_grid->item(at));
    }
    m_quiet = false;
    if (pickFirst && at >= 0)
        if (const Card *c = card(at); c && c->id != m_current) {
            m_current = c->id;
            emit picked(c->id);
        }
}

void GameCards::select(const QString &id) {
    if (id == m_current && (m_grid->currentItem() || id.isEmpty())) return;
    m_current = id;
    int k = -1;
    for (int j = 0; j < m_all.size(); j++)
        if (m_all[j].id == id) k = j;
    if (k < 0) {
        m_quiet = true;
        m_grid->setCurrentItem(nullptr);
        m_quiet = false;
        return;
    }
    if (!m_shelf.isEmpty() && m_all[k].series != m_shelf) {   // its shelf opened
        m_shelf = m_all[k].series;
        cfg::settings().setValue("ui/shelf", m_shelf);
        fillShelves();
        showShelf(false);
        return;
    }
    for (int r = 0; r < m_shown.size(); r++)
        if (m_shown[r] == k) {
            m_quiet = true;
            m_grid->setCurrentRow(r);
            m_grid->scrollToItem(m_grid->item(r));
            m_quiet = false;
        }
}

void GameCards::themeChanged() {
    fitGrid();
    // the shelves on the window's colour, the cards on the list's
    m_shelves->setStyleSheet(theme::pop() ? "QListWidget { background: palette(window); }" : QString());
    fillShelves();
    m_grid->viewport()->update();
}

void GameCards::focusCards() { m_grid->setFocus(); }

// as many cards across as the width takes, the room left shared between them
void GameCards::fitGrid() {
    QStyleOptionViewItem o;
    o.font = m_grid->font();
    const QSize card = CardPainter(this).sizeHint(o, QModelIndex()) + QSize(kGap, kGap);
    const int w = m_grid->viewport()->width() - 4;
    const int across = std::max(1, w / card.width());
    const QSize grid(std::max(card.width(), w / across), card.height());
    if (m_grid->gridSize() != grid) m_grid->setGridSize(grid);
}

bool GameCards::eventFilter(QObject *o, QEvent *e) {
    if (o == m_grid->viewport() && e->type() == QEvent::Resize) fitGrid();
    return QWidget::eventFilter(o, e);
}

GameHead::GameHead(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 10, 10, 2);
    v->setSpacing(10);
    auto *art = new HeadArt;
    art->h = &m_h;
    m_art = art;
    v->addWidget(m_art);
    m_play = new QPushButton("Play");
    m_play->setCursor(Qt::PointingHandCursor);
    m_play->setToolTip("Play this game (Enter)");
    v->addWidget(m_play);
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    m_controls = new QPushButton(QIcon::fromTheme("input-gaming"), "Controls");
    m_unlocks = new QPushButton(QIcon::fromTheme("unlock", QIcon::fromTheme("object-unlocked")), "Unlocks…");
    m_mods = new QPushButton("Mods…");
    for (QPushButton *b : {m_controls, m_unlocks, m_mods}) row->addWidget(b);
    row->addStretch(1);
    v->addLayout(row);
    connect(m_play, &QPushButton::clicked, this, &GameHead::play);
    connect(m_controls, &QPushButton::clicked, this, &GameHead::controls);
    connect(m_unlocks, &QPushButton::clicked, this, &GameHead::unlocks);
    connect(m_mods, &QPushButton::clicked, this, &GameHead::mods);
}

void GameHead::set(const Head &h) {
    m_h = h;
    m_play->setEnabled(h.canPlay);
    m_unlocks->setVisible(h.unlocks);
    themeChanged();
    static_cast<HeadArt *>(m_art)->fit();
}

void GameHead::setPlayable(bool on) {
    m_h.canPlay = on;
    m_play->setEnabled(on);
}

// Play in the series' colour (the look's corners: round in Pop, as the system's otherwise)
void GameHead::themeChanged() {
    const QPalette pal = palette();
    const QColor c = against(familyColour(m_h.card.family), pal.color(QPalette::Base));
    const bool pop = theme::pop();
    m_play->setIcon(theme::glyph("play", Qt::white));
    m_play->setIconSize(QSize(22, 22));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setBold(true);
    m_play->setFont(f);
    m_play->setStyleSheet(QString("QPushButton { background: %1; color: white; border: none; border-bottom: %2px solid %3; "
                                  "border-radius: %4px; padding: 10px 14px; font-weight: 700; }"
                                  "QPushButton:hover { background: %5; }"
                                  "QPushButton:pressed { background: %3; }"
                                  "QPushButton:disabled { background: %6; color: %7; border-bottom-color: %6; }")
                              .arg(c.name())
                              .arg(pop ? 4 : 0)
                              .arg(c.darker(135).name())
                              .arg(pop ? 16 : 4)
                              .arg(c.lighter(112).name())
                              .arg(between(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.12).name())
                              .arg(pal.color(QPalette::PlaceholderText).name()));
}
