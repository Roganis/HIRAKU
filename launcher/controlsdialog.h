// SPDX-License-Identifier: GPL-3.0-or-later
// Map keys and controller inputs to a game family's cabinet controls.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "config.h"
#include "games.h"
#include "shell/cabinet.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QMenu;
class QScrollArea;
class QToolButton;
class QWidget;
class TurntableMeter;

// The cabinet picker the Controls and Lights dialogs share: the controls sets (cfg::controlsSeries,
// the beatmania three `apart` or not: the Controls dialog's tick box, saved or not) or the families
// under a header per series (cfg::familySeries), each item's data its set or family
void fillCabinetBox(QComboBox *box, bool controlSets, bool apart = false);

class ControlsDialog : public QDialog {
    Q_OBJECT
public:
    // family: shown as its controls set (pop'n music on DJ Main, Firebeat and Python share one)
    explicit ControlsDialog(const QString &family, QWidget *parent = nullptr);
    // The games the dialog can offer to try the controls in (the ready ones of the cabinet shown).
    // Accepted with gameToTry() set: the controls are saved, and that game should start.
    void setGames(const QList<Game> &games) { m_games = games; }
    QString gameToTry() const { return m_tryGame; }
    QString testHint() const;   // "press Test (F2) for the test menu, ..."
protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void reject() override;
private:
    struct Row {
        hk::cab::Control control;
        QLabel *label = nullptr;
        QToolButton *button = nullptr;   // click: bind; its arrow (or a right-click): add, remove, reset, speed
        TurntableMeter *meter = nullptr; // an analog control's: the bound axis turning, as the game will see it
        int section = 0;
    };
    // A group of controls under a header that folds it away (the e-AMUSEMENT 10-key starts folded)
    struct Section {
        QString group;
        QToolButton *header = nullptr;
        QWidget *body = nullptr;
    };
    using Bindings = QMap<QString, QStringList>;   // control name -> bindings
    // A controller's layout: control name (as player 1's, or a shared one) -> the bindings' rests ("button3")
    using Layout = QMap<QString, QStringList>;
    void showFamily(const QString &family);
    Bindings load(const QString &family) const;
    bool save();
    void capture(int row, bool append);
    // Bind `b` to the row, replacing the row's bindings from the same keyboard or controller unless
    // `append`. An input another control of the same game has is moved here when `move`; otherwise
    // the player is asked. False: they cancelled.
    bool bind(int row, const QString &b, bool append, bool move);
    void bindAll(int player);                       // 1, 2, or 0: everything, in the order shown
    void bindingMenu(int row, QMenu *menu);
    void setupMenu(QMenu *menu);
    QStringList &bindingsOf(int row);
    Layout defaultLayout() const;
    Layout layoutOf(const QString &pad) const;      // what the controller has now, as player 1's
    Layout savedLayout(const QString &pad) const;
    void saveLayout(const QString &pad);
    void applyLayout(const QString &pad, int player, const Layout &layout);
    void tieToControllers();                        // joy<N> -> joy[<name>] for the controllers plugged in
    void updateBanner();
    void changed();                                 // after an edit: rows, highlights, banner
    void setFolded(int section, bool folded);
    void refreshRows();   // their bindings, and the inputs two controls of one game share
    void refreshHighlights();
    void source(const QString &binding, bool on);
    void updateJoystickList();

    QComboBox *m_familyBox;
    QLabel *m_setLabel;
    QCheckBox *m_apart;                             // beatmania: a set each for DJ Main, IIDX and III
    QString m_opened;                               // the family the dialog was opened for
    QLabel *m_joyLabel;
    QLabel *m_banner;                               // a controller not set up yet, pad numbers to tie
    QLabel *m_lastLabel;                            // the last input pressed
    QScrollArea *m_scroll;
    QString m_family;                               // the controls set shown
    QVector<Row> m_rows;
    QVector<Section> m_sections;
    cfg::ControlsEdits m_sets;                      // the sets shown and changed, the beatmania three apart or not
    QMap<QString, Bindings> &m_edits = m_sets.edits;   // set -> its bindings, once shown
    QSet<QString> &m_dirty = m_sets.dirty;          // sets changed since the last save
    QSet<QString> m_active;                         // sources held down now (canonicalBinding)
    QSet<QString> m_hidden;                         // controllers the banner was told to leave out (settings)
    QList<Game> m_games;
    QString m_tryGame;
    QHash<QString, qint64> m_flashUntil;            // analog axes: lit briefly after motion
    QHash<QString, int> m_hats;                     // "joyN:hatK" -> SDL hat bits
    QSet<QString> m_restNeg, m_restPos;             // axes resting past a button threshold, as the game takes them
    QElapsedTimer m_clock;
    QTimer m_flashTimer;
    bool m_capturing = false;
};
