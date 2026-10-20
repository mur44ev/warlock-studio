// SPDX-License-Identifier: GPL-3.0-or-later
// Warlock Studio: Windows 10 "Metro" front end (flat tiles, Segoe UI Light, accent colour).
#include <QtWidgets>

#include <QJsonArray>
#include <QJsonDocument>
#include <QDesktopServices>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <functional>

#include "../PresetFormat.h"
#include "Device.h"
#include "WarlockLayout.h"

namespace {

// ---- Design language ----------------------------------------------------------------------
const QColor kBack("#1F1F1F"), kSide("#171717"), kPanel("#2B2B2B"), kRaised("#333333"), kLine("#434343");
const QColor kText("#FFFFFF"), kSub("#A6A6A6");

QFont uiFont(double pt, const char* family = "Segoe UI") { QFont f(family); f.setPointSizeF(pt); return f; }
QFont iconFont(int px) { QFont f("Segoe MDL2 Assets"); f.setPixelSize(px); return f; }
QColor shade(const QColor& c, int percent) { return percent >= 0 ? c.lighter(100 + percent) : c.darker(100 - percent); }
QString glyph(ushort code) { return QString(QChar(code)); }

const char* const kAccents[] = {"#0078D7", "#0099BC", "#00B294", "#10893E", "#498205", "#FFB900", "#F7630C",
                                "#E81123", "#E3008C", "#B146C2", "#8764B8", "#69797E"};

QString metroStyleSheet(const QColor& accent) {
    QString css = R"(
QMainWindow,QWidget{background:#1F1F1F;color:#FFFFFF;}
QLabel,QCheckBox,QWidget#flat{background:transparent;}
QScrollArea,QScrollArea>QWidget>QWidget{background:transparent;border:0;}
QScrollBar:vertical{background:transparent;width:12px;margin:0;}
QScrollBar::handle:vertical{background:#505050;min-height:36px;margin:0 3px;}
QScrollBar::handle:vertical:hover{background:#787878;}
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}
QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}
QPushButton{background:#333333;border:2px solid transparent;padding:7px 18px;min-height:20px;color:#FFFFFF;}
QPushButton:hover{border-color:#8A8A8A;}
QPushButton:pressed{background:#5C5C5C;}
QPushButton:disabled{background:#262626;color:#6E6E6E;}
QPushButton[primary="true"]{background:@ACCENT@;}
QPushButton[primary="true"]:hover{background:@ACCENT_L@;border-color:#FFFFFF;}
QPushButton[primary="true"]:pressed{background:@ACCENT_D@;}
QComboBox,QSpinBox,QLineEdit,QTextEdit{background:#333333;border:2px solid #333333;padding:5px 8px;selection-background-color:@ACCENT@;}
QComboBox:hover,QSpinBox:hover,QLineEdit:hover,QTextEdit:hover{border-color:#8A8A8A;}
QComboBox:focus,QSpinBox:focus,QLineEdit:focus,QTextEdit:focus{border-color:@ACCENT@;}
QComboBox::drop-down{border:0;width:26px;}
QComboBox QAbstractItemView{background:#2B2B2B;border:1px solid #5A5A5A;selection-background-color:@ACCENT@;outline:0;}
QSpinBox::up-button,QSpinBox::down-button{width:20px;border:0;background:#3E3E3E;}
QSpinBox::up-button:hover,QSpinBox::down-button:hover{background:#5A5A5A;}
QCheckBox{spacing:10px;}
QCheckBox::indicator{width:18px;height:18px;border:2px solid #CFCFCF;background:transparent;}
QCheckBox::indicator:hover{border-color:#FFFFFF;}
QCheckBox::indicator:checked{background:@ACCENT@;border-color:@ACCENT@;}
QHeaderView::section{background:#333333;padding:6px;border:0;}
QToolTip{background:#2B2B2B;color:#FFFFFF;border:1px solid #5A5A5A;padding:4px;}
)";
    return css.replace("@ACCENT@", accent.name()).replace("@ACCENT_L@", shade(accent, 18).name()).replace("@ACCENT_D@", shade(accent, -22).name());
}

struct EffectEntry { int mode; const char* label; ushort icon; const char* tint; bool reactive; };
constexpr int kReactiveStart = AW60_MODE_FIRST_EFFECT + AW60_FIRST_REACTIVE;
const EffectEntry kEffects[] = {
    {AW60_MODE_FIRST_EFFECT + 0, "Rainbow Wave", 0xE706, "#C30052", false},
    {AW60_MODE_FIRST_EFFECT + 1, "Color Wave", 0xE706, "#9A0089", false},
    {AW60_MODE_FIRST_EFFECT + 2, "Gradient", 0xE790, "#5D5A58", false},
    {AW60_MODE_FIRST_EFFECT + 3, "Breathing", 0xE706, "#0063B1", false},
    {AW60_MODE_FIRST_EFFECT + 4, "Twinkle", 0xE734, "#2D7D9A", false},
    {AW60_MODE_FIRST_EFFECT + 5, "Scanner", 0xE768, "#038387", false},
    {AW60_MODE_FIRST_EFFECT + 6, "Aurora Drift", 0xE706, "#018574", false},
    {AW60_MODE_FIRST_EFFECT + 7, "Plasma Cloud", 0xE790, "#744DA9", false},
    {AW60_MODE_FIRST_EFFECT + 8, "Sunset Drift", 0xE706, "#CA5010", false},
    {AW60_MODE_FIRST_EFFECT + 9, "Ocean Tide", 0xE706, "#0078D7", false},
    {AW60_MODE_FIRST_EFFECT + 10, "Ember Rain", 0xE734, "#DA3B01", false},
    {AW60_MODE_FIRST_EFFECT + 11, "Comet Trail", 0xE768, "#486860", false},
    {AW60_MODE_FIRST_EFFECT + 12, "Kaleido Mirror", 0xE790, "#881798", false},
    {AW60_MODE_FIRST_EFFECT + 13, "Velvet Wave", 0xE706, "#6B69D6", false},
    {AW60_MODE_FIRST_EFFECT + 14, "Key Glow", 0xE765, "#E3008C", true},
    {AW60_MODE_FIRST_EFFECT + 15, "Key Ripple", 0xE765, "#0099BC", true},
    {AW60_MODE_FIRST_EFFECT + 16, "Laser Cross", 0xE765, "#E81123", true},
    {AW60_MODE_FIRST_EFFECT + 17, "Heat Trail", 0xE765, "#F7630C", true},
    {AW60_MODE_FIRST_EFFECT + 18, "Fireworks", 0xE765, "#8E8CD8", true},
};

bool isEffectMode(int mode) { return mode >= AW60_MODE_FIRST_EFFECT && mode < AW60_MODE_FIRST_EFFECT + AW60_EFFECT_COUNT; }
bool isReactiveMode(int mode) { return mode >= kReactiveStart && mode < AW60_MODE_FIRST_EFFECT + AW60_EFFECT_COUNT; }
QString effectTip(int mode) {
    switch (mode) {
    case AW60_MODE_DIRECT: return "Каждая клавиша получает свой цвет. Выделите клавиши на схеме и нажмите «Окрасить».";
    case AW60_MODE_OFF: return "Подсветка выключена.";
    case AW60_MODE_HARDWARE: return "Штатные эффекты прошивки (Ripple, Wave, Rain…). Сохраняются в клавиатуре.";
    case AW60_MODE_FIRST_EFFECT + 14: return "Нажатая клавиша вспыхивает и плавно гаснет. Зажатая горит, пока удерживается.";
    case AW60_MODE_FIRST_EFFECT + 15: return "От каждой нажатой клавиши расходится кольцо.";
    case AW60_MODE_FIRST_EFFECT + 16: return "От нажатой клавиши лучи бегут по её ряду и столбцу.";
    case AW60_MODE_FIRST_EFFECT + 17: return "Часто нажимаемые клавиши нагреваются: от основного цвета к блику. Остывают медленно.";
    case AW60_MODE_FIRST_EFFECT + 18: return "Нажатие запускает фейерверк из искр разных цветов палитры.";
    default: return "Эффект считается прямо в клавиатуре и работает без программы.";
    }
}
int legacyMode(int saved) { return saved == 7 ? AW60_MODE_OFF : saved == 8 ? AW60_MODE_HARDWARE : saved; }
bool validPreset(const QByteArray& blob) {
    return blob.size() == AW60_PRESET_SLOT_BYTES && blob.startsWith(AW60_PRESET_MAGIC) && quint8(blob[4]) == AW60_PRESET_VERSION &&
           quint8(blob[AW60_PRESET_MODE_OFFSET]) < AW60_EFFECT_COUNT;
}
QString presetName(const QByteArray& blob) {
    return QString::fromUtf8(blob.mid(AW60_PRESET_NAME_OFFSET, AW60_PRESET_NAME_BYTES).split('\0').value(0));
}

QLabel* text(const QString& s, double pt = 10, const QColor& color = kSub, const char* family = "Segoe UI") {
    auto l = new QLabel(s);
    l->setWordWrap(true);
    l->setFont(uiFont(pt, family));
    l->setStyleSheet(QString("color:%1;").arg(color.name()));
    return l;
}
QLabel* heading(const QString& s) { return text(s, 11, kText, "Segoe UI Semibold"); }
QPushButton* button(const QString& s, std::function<void()> f, bool primary = false) {
    auto b = new QPushButton(s);
    b->setProperty("primary", primary);
    b->setCursor(Qt::PointingHandCursor);
    QObject::connect(b, &QPushButton::clicked, b, [f] { f(); });
    return b;
}
void swatch(QPushButton* b, const QColor& c) {
    QString fg = c.lightness() > 150 ? "#101010" : "#FFFFFF";
    b->setText(c.name().toUpper());
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:2px solid transparent;padding:10px 6px;min-height:26px;}"
                             "QPushButton:hover{border-color:#FFFFFF;}").arg(c.name(), fg));
}

// ---- Widgets --------------------------------------------------------------------------------
// Windows 10 slider: 2 px track, accent-coloured value part, pill handle.
class MetroSlider : public QSlider {
public:
    explicit MetroSlider(QWidget* p = nullptr) : QSlider(Qt::Horizontal, p) { setFixedHeight(28); setCursor(Qt::PointingHandCursor); }
    void paintEvent(QPaintEvent*) override {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        QColor accent(qApp->property("accent").toString());
        if (!accent.isValid()) accent = QColor("#0078D7");
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        int cy = height() / 2, cx = handle.center().x();
        bool on = isEnabled();
        p.fillRect(QRect(4, cy - 1, width() - 8, 2), QColor(on ? "#6B6B6B" : "#444444"));
        p.fillRect(QRect(4, cy - 1, qMax(0, cx - 4), 2), on ? accent : QColor("#555555"));
        QColor knob = !on ? QColor("#777777") : (isSliderDown() ? accent : underMouse() ? shade(accent, 25) : QColor("#FFFFFF"));
        p.setPen(Qt::NoPen);
        p.setBrush(knob);
        p.drawRoundedRect(QRectF(cx - 4, cy - 11, 8, 22), 4, 4);
    }
};

class Keyboard : public QWidget {
public:
    enum Kind { Lighting, Mapping, Monitor };
    QVector<QColor> colors = QVector<QColor>(80, QColor("#0088cc"));
    QSet<int> selected, pressed;
    std::function<void()> changed;
    QColor accent = QColor("#0078D7");
    Kind kind;
    explicit Keyboard(Kind k, QWidget* p = nullptr) : QWidget(p), kind(k) {
        setMinimumSize(600, 230);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }
    QRectF keyRect(int k) const {
        int row = 0, col = 0;
        for (int i = 0; i < 192; ++i)
            if (WarlockLayout::matrix[i] == unsigned(k)) { row = i / 32; col = i % 32; break; }
        double u = (width() - 24) / 32., h = qMin((height() - 24) / 6., u * 2.5);
        double w = k == 74 ? 6.6 : 1.8;
        return QRectF(12 + col * u, 12 + row * h, w * u - 4, h - 6);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#181818"));
        for (int i = 0; i < 80; ++i) {
            auto r = keyRect(i);
            QColor fill = kind == Lighting ? colors[i] : QColor("#333333");
            if (kind == Monitor && pressed.contains(i)) fill = accent;
            if (kind == Mapping && selected.contains(i)) fill = accent;
            if (i == hover && kind != Monitor) fill = shade(fill, 22);
            p.fillRect(r, fill);
            if (kind == Lighting) {
                p.setPen(QPen(selected.contains(i) ? QColor("#FFFFFF") : QColor("#4A4A4A"), selected.contains(i) ? 3 : 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(1, 1, -1, -1));
            }
            bool light = fill.lightness() > 150;
            p.setPen(light ? QColor("#101010") : QColor("#FFFFFF"));
            p.setFont(uiFont(i == 74 ? 10 : 8));
            QString name = QString::fromLatin1(WarlockLayout::keys[i].name);
            name.replace("Caps Lock", "Caps").replace("Left ", "L ").replace("Right ", "R ").replace("BackSpace", "Back").replace("Page Down", "PgDn").replace("Page Up", "PgUp");
            p.drawText(r.adjusted(1, 0, -1, 0), Qt::AlignCenter, name);
        }
    }
    int keyAt(const QPoint& pos) const {
        for (int i = 0; i < 80; ++i) if (keyRect(i).contains(pos)) return i;
        return -1;
    }
    void mouseMoveEvent(QMouseEvent* e) override { int k = keyAt(e->pos()); if (k != hover) { hover = k; update(); } }
    void leaveEvent(QEvent*) override { hover = -1; update(); }
    void mousePressEvent(QMouseEvent* e) override {
        if (kind == Monitor) return;
        int i = keyAt(e->pos());
        if (i < 0) return;
        if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) selected.clear();
        if (selected.contains(i)) selected.remove(i); else selected.insert(i);
        update();
        if (changed) changed();
    }
private:
    int hover = -1;
};

// Flat coloured tile in the Start-screen style: icon top-left, caption bottom-left.
class Tile : public QAbstractButton {
public:
    ushort icon;
    QColor tint;
    QString badge;
    Tile(const QString& label, ushort glyphCode, const QColor& color, const QString& tag = QString(), QWidget* p = nullptr)
        : QAbstractButton(p), icon(glyphCode), tint(color), badge(tag) {
        setText(label);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(84, 64);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        QColor fill = tint;
        if (isDown()) fill = shade(fill, -18); else if (underMouse()) fill = shade(fill, 14);
        p.fillRect(rect(), fill);
        p.setPen(Qt::white);
        p.setFont(iconFont(20));
        p.drawText(QRect(8, 3, 32, 28), Qt::AlignLeft | Qt::AlignVCenter, glyph(icon));
        p.setFont(uiFont(8.5));
        p.drawText(QRect(8, 32, width() - 12, 28), Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, text());
        if (!badge.isEmpty()) {
            p.setFont(uiFont(7, "Segoe UI Semibold"));
            QRect b(width() - 32, 6, 26, 14);
            p.fillRect(b, QColor(0, 0, 0, 90));
            p.drawText(b, Qt::AlignCenter, badge);
        }
        if (isChecked()) {
            p.setPen(QPen(Qt::white, 3));
            p.setBrush(Qt::NoBrush);
            p.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }
};

class NavButton : public QAbstractButton {
public:
    ushort icon;
    QColor accent = QColor("#0078D7");
    NavButton(const QString& label, ushort glyphCode, QWidget* p = nullptr) : QAbstractButton(p), icon(glyphCode) {
        setText(label);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(48);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), isChecked() ? QColor("#2D2D2D") : underMouse() ? QColor("#262626") : QColor("#171717"));
        if (isChecked()) p.fillRect(0, 8, 4, height() - 16, accent);
        p.setPen(Qt::white);
        p.setFont(iconFont(16));
        p.drawText(QRect(0, 0, 52, height()), Qt::AlignCenter, glyph(icon));
        p.setFont(uiFont(11, isChecked() ? "Segoe UI Semibold" : "Segoe UI"));
        p.drawText(QRect(52, 0, width() - 56, height()), Qt::AlignLeft | Qt::AlignVCenter, text());
    }
};

}  // namespace

class Window : public QMainWindow {
    Device* device = new Device;
    QThread usb;
    QStackedWidget* pages = new QStackedWidget;
    QButtonGroup* navGroup = new QButtonGroup(this);
    QVector<NavButton*> navButtons;
    QButtonGroup* tileGroup = new QButtonGroup(this);
    QMap<int, Tile*> tiles;
    QLabel *status = text("Подключение…"), *link = text("Не подключено", 9), *fps = text("—", 40, kText, "Segoe UI Light"),
           *selection = text("Выберите клавиши на схеме"), *usbResult = text("Нажмите «Проверить USB»"),
           *actionInfo = text("Выберите клавишу, чтобы увидеть текущее назначение"), *effectHint = text(""),
           *monitorInfo = text("Монитор выключен");
    Keyboard *lighting = new Keyboard(Keyboard::Lighting), *mapping = new Keyboard(Keyboard::Mapping), *monitor = new Keyboard(Keyboard::Monitor);
    QComboBox *mode = new QComboBox, *hardware = new QComboBox, *target = new QComboBox, *actionType = new QComboBox, *layer = new QComboBox;
    QSlider *brightness = new MetroSlider, *speed = new MetroSlider, *fpsLimit = new MetroSlider, *effectScale = new MetroSlider, *effectAmount = new MetroSlider;
    QSpinBox *debounce = new QSpinBox, *nativeProfile = new QSpinBox, *presetSlot = new QSpinBox;
    QLineEdit* presetNameEdit = new QLineEdit;
    QLabel *effectName = text("", 11, kText, "Segoe UI Semibold"), *perfOut = text("Нет данных. Нажмите «Сбросить счётчики», поработайте за клавиатурой, затем «Снять замеры».", 9, kText, "Consolas");
    QVector<Tile*> slotTiles;
    QVector<QByteArray> slotRaw = QVector<QByteArray>(AW60_PRESET_SLOT_COUNT);
    QPushButton *pickA = new QPushButton, *pickB = new QPushButton, *pickC = new QPushButton, *monitorToggle = nullptr;
    QWidget *hardwareRow = nullptr, *effectBox = nullptr;
    QColor color = QColor("#00aaff"), color2 = QColor("#ff4fd8"), color3 = QColor("#7bffef"), accent = QColor("#0078D7");
    QJsonObject info;
    bool preferRadio = false;
    QSettings settings;

    int currentMode() const { return mode->currentData().toInt(); }
    void selectMode(int value) {
        for (int i = 0; i < mode->count(); ++i)
            if (mode->itemData(i).toInt() == value) { mode->setCurrentIndex(i); break; }
        if (tiles.contains(value)) tiles[value]->setChecked(true);
        updateModeUi();
    }
    void updateColorButtons() { swatch(pickA, color); swatch(pickB, color2); swatch(pickC, color3); }
    void updateModeUi() {
        int m = currentMode();
        bool effect = isEffectMode(m);
        hardwareRow->setVisible(m == AW60_MODE_HARDWARE);
        effectBox->setEnabled(effect || m == AW60_MODE_DIRECT || m == AW60_MODE_OFF);
        effectScale->setEnabled(effect);
        effectAmount->setEnabled(effect);
        pickB->setEnabled(effect);
        pickC->setEnabled(effect);
        QString hint;
        if (isReactiveMode(m)) hint = "Реагирует на нажатия клавиш и считается прямо в клавиатуре — Studio после включения можно закрыть. Нужна прошивка rev6.";
        else if (effect) hint = "Эффект считается в клавиатуре и работает без программы.";
        else if (m == AW60_MODE_DIRECT) hint = "Цвета выбранных клавиш задаются вручную и передаются по кабелю.";
        else if (m == AW60_MODE_HARDWARE) hint = "Штатный эффект прошивки сохраняется в клавиатуре.";
        effectHint->setText(hint);
        effectName->setText(tiles.contains(m) ? tiles[m]->text() : QString());
    }
    QJsonObject state() {
        QJsonArray c;
        for (auto v : lighting->colors) c.append(v.name());
        return {{"mode", currentMode()}, {"hardware", hardware->currentData().toInt()}, {"color", color.name()}, {"color2", color2.name()},
                {"color3", color3.name()}, {"brightness", brightness->value()}, {"speed", speed->value()}, {"fps", fpsLimit->value()},
                {"scale", effectScale->value()}, {"amount", effectAmount->value()}, {"colors", c}};
    }
    void post(std::function<void()> f) { QMetaObject::invokeMethod(device, [f] { f(); }, Qt::QueuedConnection); }
    void apply() { auto s = state(); notice("Применяется…"); post([this, s] { device->apply(s); }); }
    void setLink(bool ok, const QString& detail = QString()) {
        link->setTextFormat(Qt::RichText);
        link->setText(QString("<span style='color:%1;font-size:12pt'>&#9679;</span>&nbsp; <b>%2</b><br><span style='color:#A6A6A6'>%3</span>")
                          .arg(ok ? "#2DC26B" : "#E5484D", ok ? "Подключено" : "Нет связи", detail.toHtmlEscaped().replace("\n", "<br>")));
    }
    void notice(const QString& s, bool error = false) {
        status->setText(s);
        status->setStyleSheet(QString("color:%1;padding:10px 16px;background:%2;font-size:10pt;").arg(error ? "#FFB4B4" : "#FFFFFF", error ? "#A4262C" : "#2B2B2B"));
    }

    void showPerf(const QJsonObject& r) {
        double cpm = qMax(1., r["cyclesPerMs"].toDouble()), elapsed = r["elapsedMs"].toDouble(), polls = r["polls"].toDouble();
        QString t;
        t += QString("Ядро: %1 МГц (HCLK sel %2, SysTick %3)\n").arg(cpm / 1000., 0, 'f', 0).arg(r["hclkSel"].toDouble(), 0, 'f', 0).arg(r["reload"].toDouble(), 0, 'f', 0);
        if (elapsed > 0) t += QString("Интервал замера: %1 с\n").arg(elapsed / 1000., 0, 'f', 1);
        else t += "Интервал неизвестен (счётчики с включения клавиатуры): процент загрузки не показан.\n";
        double entries = polls + r["scans"].toDouble();
        t += QString("\nВходы в наши хуки (главный цикл + обновление подсветки): %1").arg(entries, 0, 'f', 0);
        if (elapsed > 0) t += QString(", %1 в секунду").arg(entries * 1000. / elapsed, 0, 'f', 0);
        t += QString("\n  самая долгая пауза между входами: %1 мс\n").arg(r["gapMax"].toDouble() / cpm, 0, 'f', 3);
        static const char* const bins[8] = {"< 0.1 мс", "< 0.25 мс", "< 0.5 мс", "< 1 мс", "< 2 мс", "< 5 мс", "< 10 мс", "≥ 10 мс"};
        double total = 0;
        for (auto v : r["hist"].toArray()) total += v.toDouble();
        auto hist = r["hist"].toArray();
        for (int i = 0; i < 8; ++i) {
            double share = total > 0 ? hist[i].toDouble() * 100. / total : 0;
            t += QString("  %1 %2 %3%\n").arg(QString::fromUtf8(bins[i]), -10).arg(QString(int(share / 4), QChar(0x2588)), -26).arg(share, 0, 'f', 1);
        }
        static const char* const names[4] = {"USB", "Таймер 1", "Таймер 2 (скан клавиш)", "Таймер 3"};
        t += "\nПрерывания:\n";
        auto isr = r["isr"].toArray();
        for (int i = 0; i < 4; ++i) {
            auto o = isr[i].toObject();
            double count = o["count"].toDouble(), sum = o["sum16"].toDouble() * 16.;
            QString load = elapsed > 0 ? QString("%1% ЦП").arg(sum * 100. / (elapsed * cpm), 0, 'f', 1) : QString();
            t += QString("  %1 вызовов %2, макс %3 мкс, средн %4 мкс  %5\n").arg(QString::fromUtf8(names[i]), -24).arg(count, 0, 'f', 0)
                     .arg(o["max"].toDouble() * 1000. / cpm, 0, 'f', 0).arg(count > 0 ? sum * 1000. / cpm / count : 0., 0, 'f', 0).arg(load);
        }
        t += QString("\nКадры эффектов: %1, кусков %2, самый долгий кусок %3 мкс\n").arg(r["frames"].toDouble(), 0, 'f', 0).arg(r["slices"].toDouble(), 0, 'f', 0).arg(r["sliceMax"].toDouble() * 1000. / cpm, 0, 'f', 0);
        t += QString("Конвертер стока: %1 вызовов, макс %2 мкс\n").arg(r["convCount"].toDouble(), 0, 'f', 0).arg(r["convMax"].toDouble() * 1000. / cpm, 0, 'f', 0);
        perfOut->setText(t);
        notice("Замеры сняты");
    }

    // ---- layout helpers ----
    QVBoxLayout* page(const QString& title, const QString& sub) {
        auto scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto body = new QWidget;
        body->setObjectName("flat");
        auto l = new QVBoxLayout(body);
        l->setContentsMargins(32, 24, 32, 24);
        l->setSpacing(16);
        l->addWidget(text(title, 30, kText, "Segoe UI Light"));
        if (!sub.isEmpty()) l->addWidget(text(sub, 10, kSub));
        scroll->setWidget(body);
        pages->addWidget(scroll);
        return l;
    }
    QWidget* row(std::initializer_list<QWidget*> widgets, int spacing = 12) {
        auto w = new QWidget;
        w->setObjectName("flat");
        auto l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(spacing);
        for (auto x : widgets) l->addWidget(x);
        return w;
    }
    QWidget* panel(QVBoxLayout*& inner, int pad = 20) {
        auto w = new QWidget;
        w->setObjectName("panel");
        w->setStyleSheet("#panel{background:#2B2B2B;}");
        inner = new QVBoxLayout(w);
        inner->setContentsMargins(pad, pad - 4, pad, pad);
        inner->setSpacing(12);
        return w;
    }
    QWidget* sliderRow(const QString& title, QSlider* slider, std::function<QString(int)> format) {
        auto w = new QWidget;
        w->setObjectName("flat");
        auto g = new QVBoxLayout(w);
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(2);
        auto head = new QHBoxLayout;
        auto value = text("", 10, kText);
        value->setAlignment(Qt::AlignRight);
        head->addWidget(text(title, 10, kSub));
        head->addWidget(value);
        g->addLayout(head);
        g->addWidget(slider);
        auto update = [slider, value, format] { value->setText(format(slider->value())); };
        connect(slider, &QSlider::valueChanged, w, update);
        update();
        return w;
    }
    void setAccent(const QColor& c) {
        accent = c;
        qApp->setProperty("accent", c.name());
        settings.setValue("accent", c.name());
        qApp->setStyleSheet(metroStyleSheet(c));
        for (auto b : navButtons) { b->accent = c; b->update(); }
        for (auto k : {lighting, mapping, monitor}) { k->accent = c; k->update(); }
    }

    // ---- preset blobs ----
    QByteArray encodePreset() const {
        QByteArray blob(AW60_PRESET_SLOT_BYTES, '\0');
        blob.replace(0, 4, AW60_PRESET_MAGIC);
        blob[4] = char(AW60_PRESET_VERSION);
        blob[AW60_PRESET_MODE_OFFSET] = char(currentMode() - AW60_MODE_FIRST_EFFECT);
        blob[AW60_PRESET_SPEED_OFFSET] = char(speed->value());
        blob[AW60_PRESET_BRIGHTNESS_OFFSET] = char(brightness->value());
        blob[AW60_PRESET_FPS_OFFSET] = char(fpsLimit->value());
        blob[AW60_PRESET_SCALE_OFFSET] = char(effectScale->value());
        blob[AW60_PRESET_AMOUNT_OFFSET] = char(effectAmount->value());
        blob[AW60_PRESET_FLAGS_OFFSET] = char(AW60_FLAG_EXPLICIT_PALETTE);
        const QColor cols[3] = {color, color2, color3};
        const int offs[3] = {AW60_PRESET_COLOR1_OFFSET, AW60_PRESET_COLOR2_OFFSET, AW60_PRESET_COLOR3_OFFSET};
        for (int i = 0; i < 3; ++i) { blob[offs[i]] = char(cols[i].red()); blob[offs[i] + 1] = char(cols[i].green()); blob[offs[i] + 2] = char(cols[i].blue()); }
        auto name = presetNameEdit->text().trimmed().toUtf8().left(AW60_PRESET_NAME_BYTES - 1);
        for (int i = 0; i < name.size(); ++i) blob[AW60_PRESET_NAME_OFFSET + i] = name[i];
        return blob;
    }
    bool applyPresetBlob(const QByteArray& blob, bool announce = false) {
        if (!validPreset(blob)) { if (announce) notice("Слот пустой или это не AW60-пресет", true); return false; }
        auto byteAt = [&](int i) { return int(quint8(blob[i])); };
        selectMode(byteAt(AW60_PRESET_MODE_OFFSET) + AW60_MODE_FIRST_EFFECT);
        speed->setValue(qBound(1, byteAt(AW60_PRESET_SPEED_OFFSET), 100));
        brightness->setValue(byteAt(AW60_PRESET_BRIGHTNESS_OFFSET));
        fpsLimit->setValue(qBound(1, byteAt(AW60_PRESET_FPS_OFFSET), 60));
        effectScale->setValue(qBound(1, byteAt(AW60_PRESET_SCALE_OFFSET) ? byteAt(AW60_PRESET_SCALE_OFFSET) : 50, 100));
        effectAmount->setValue(qBound(1, byteAt(AW60_PRESET_AMOUNT_OFFSET) ? byteAt(AW60_PRESET_AMOUNT_OFFSET) : 60, 100));
        color = QColor(byteAt(AW60_PRESET_COLOR1_OFFSET), byteAt(AW60_PRESET_COLOR1_OFFSET + 1), byteAt(AW60_PRESET_COLOR1_OFFSET + 2));
        color2 = QColor(byteAt(AW60_PRESET_COLOR2_OFFSET), byteAt(AW60_PRESET_COLOR2_OFFSET + 1), byteAt(AW60_PRESET_COLOR2_OFFSET + 2));
        color3 = QColor(byteAt(AW60_PRESET_COLOR3_OFFSET), byteAt(AW60_PRESET_COLOR3_OFFSET + 1), byteAt(AW60_PRESET_COLOR3_OFFSET + 2));
        presetNameEdit->setText(presetName(blob));
        updateColorButtons();
        updateModeUi();
        if (announce) notice("Слот загружен в редактор. Нажмите «Включить», чтобы применить его к клавиатуре.");
        return true;
    }

    void save(const QString& path) {
        QJsonObject s = state();
        s["format"] = "WarlockStudio2";
        s["presetSlot"] = presetSlot->value();
        s["presetName"] = presetNameEdit->text();
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly)) { notice(f.errorString(), true); return; }
        f.write(QJsonDocument(s).toJson());
        if (!f.commit()) notice("Не удалось сохранить профиль", true); else notice("Профиль сохранён: " + path);
    }
    void load(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { notice(f.errorString(), true); return; }
        QJsonParseError err;
        auto s = QJsonDocument::fromJson(f.readAll(), &err).object();
        auto format = s["format"].toString();
        if (err.error != QJsonParseError::NoError || (format != "WarlockStudio1" && format != "WarlockStudio2") || s["colors"].toArray().size() != 80) {
            notice("Неверный формат профиля", true);
            return;
        }
        bool legacy = format == "WarlockStudio1";
        int savedMode = s["mode"].toInt(AW60_MODE_DIRECT);
        selectMode(legacy ? legacyMode(savedMode) : savedMode);
        brightness->setValue(s["brightness"].toInt(255));
        speed->setValue(s["speed"].toInt(50));
        fpsLimit->setValue(s["fps"].toInt(60));
        effectScale->setValue(s["scale"].toInt(50));
        effectAmount->setValue(s["amount"].toInt(60));
        color = QColor(s["color"].toString("#00aaff"));
        color2 = QColor(s["color2"].toString("#ff4fd8"));
        color3 = QColor(s["color3"].toString("#7bffef"));
        auto c = s["colors"].toArray();
        for (int i = 0; i < 80; ++i) lighting->colors[i] = QColor(c[i].toString());
        lighting->update();
        presetSlot->setValue(s["presetSlot"].toInt());
        presetNameEdit->setText(s["presetName"].toString());
        updateColorButtons();
        updateModeUi();
        notice("Профиль загружен. Нажмите «Включить» для подсветки; аппаратные слои и слоты пресетов записываются отдельно.");
    }

    // ---- key remapping ----
    void selectedAction() {
        auto keys = mapping->selected;
        if (keys.size() != 1) { actionInfo->setText("Выбрано клавиш: " + QString::number(keys.size())); return; }
        int k = *keys.begin(), slot = int(WarlockLayout::keys[k].slot);
        auto b = QByteArray::fromHex(info[layer->currentIndex() ? "fn" : "base"].toString().toLatin1());
        if (b.size() == 512)
            actionInfo->setText(QString("%1 • текущее действие: %2").arg(QString::fromLatin1(WarlockLayout::keys[k].name), QString(b.mid(slot * 4, 4).toHex(' '))));
    }
    void applyKeys() {
        QByteArray value(4, 0);
        int type = actionType->currentIndex();
        if (type == 0) value[2] = char(target->currentData().toInt());
        else if (type == 1) { value[0] = 3; unsigned u = target->currentData().toUInt(); value[2] = char(u); value[3] = char(u >> 8); }
        QJsonArray keys;
        for (int k : mapping->selected) keys.append(k);
        QJsonObject r{{"fn", layer->currentIndex() == 1}, {"keys", keys}, {"value", QString(value.toHex())}};
        post([this, r] { device->writeKeys(r); });
        notice("Запись назначений…");
    }
    void restoreBackup() {
        QString p = QFileDialog::getOpenFileName(this, "Восстановить резервную копию", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups", "Backup (*.json)");
        if (p.isEmpty()) return;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) { notice(f.errorString(), true); return; }
        auto o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.contains("base") && o.contains("fn")) {
            for (auto name : {QString("base"), QString("fn")}) {
                QJsonObject r{{"image", o[name]}, {"profile", o["profile"]}, {"fn", name == "fn"}};
                post([this, r] { device->writeKeys(r); });
            }
        } else if (o["bytes"].toString().size() == 1024) {
            QJsonObject r{{"image", o["bytes"]}, {"profile", o["profile"]}, {"fn", o["fn"]}};
            post([this, r] { device->writeKeys(r); });
        } else if (o["preset"].toBool() && o["bytes"].toString().size() == AW60_PRESET_SLOT_BYTES * 2 && o.contains("slot")) {
            QJsonObject r{{"slot", o["slot"]}, {"raw", o["bytes"]}};
            post([this, r] { device->writePreset(r); });
            notice("Восстановление пресета…");
        } else notice("Это не резервная копия слоя или пресета", true);
    }
    void targets() {
        target->clear();
        if (actionType->currentIndex() == 1) {
            target->addItem("Громкость +", 0xe9); target->addItem("Громкость −", 0xea); target->addItem("Без звука", 0xe2);
            target->addItem("Воспроизведение / пауза", 0xcd); target->addItem("Следующий трек", 0xb5); target->addItem("Предыдущий трек", 0xb6);
        } else if (actionType->currentIndex() == 0) {
            static const QMap<int, QString> names{{40, "Enter"}, {41, "Escape"}, {42, "Backspace"}, {43, "Tab"}, {44, "Space"}, {73, "Insert"}, {74, "Home"},
                {75, "Page Up"}, {76, "Delete"}, {77, "End"}, {78, "Page Down"}, {79, "Right"}, {80, "Left"}, {81, "Down"}, {82, "Up"},
                {224, "Left Ctrl"}, {225, "Left Shift"}, {226, "Left Alt"}, {227, "Left Win"}};
            for (int i = 4; i <= 0xe7; ++i) {
                QString name = QString("HID 0x%1").arg(i, 2, 16, QChar('0'));
                if (i <= 29) name = QString(QChar('A' + i - 4));
                else if (i >= 30 && i <= 38) name = QString::number(i - 29);
                else if (i == 39) name = "0";
                else if (i >= 58 && i <= 69) name = "F" + QString::number(i - 57);
                else if (names.contains(i)) name = names[i];
                target->addItem(name, i);
            }
        }
        target->setVisible(actionType->currentIndex() != 2);
    }

    // ---- pages ----
    void buildLighting() {
        auto outer = page("Подсветка", "Выберите эффект плиткой, настройте параметры справа и нажмите «Включить» (Ctrl+Enter). Клавиши на схеме выделяются мышью, Ctrl или Shift — несколько сразу, Ctrl+A — все.");
        auto cols = new QHBoxLayout;
        cols->setSpacing(24);
        auto left = new QVBoxLayout;
        left->setSpacing(14);
        left->addWidget(lighting, 1);
        lighting->changed = [this] { selection->setText(QString("Выбрано клавиш: %1").arg(lighting->selected.size())); };
        left->addWidget(selection);
        effectName->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        left->addWidget(row({heading("Эффекты"), effectName}));

        mode->addItem("Свои цвета / Direct", AW60_MODE_DIRECT);
        for (const auto& e : kEffects) mode->addItem(QString::fromUtf8(e.label), e.mode);
        mode->addItem("Выключено", AW60_MODE_OFF);
        mode->addItem("Аппаратный эффект", AW60_MODE_HARDWARE);

        auto grid = new QGridLayout;
        grid->setSpacing(6);
        int index = 0;
        auto addTile = [&](int modeId, const QString& label, ushort icon, const QColor& tint, const QString& badge) {
            auto t = new Tile(label, icon, tint, badge);
            tileGroup->addButton(t);
            tiles[modeId] = t;
            t->setToolTip(effectTip(modeId));
            connect(t, &QAbstractButton::clicked, this, [this, modeId] { selectMode(modeId); });
            grid->addWidget(t, index / 8, index % 8);
            ++index;
        };
        addTile(AW60_MODE_DIRECT, "Свои цвета", 0xE790, QColor("#515C6B"), "");
        for (const auto& e : kEffects) addTile(e.mode, QString::fromUtf8(e.label), e.icon, QColor(e.tint), e.reactive ? "KEY" : "");
        addTile(AW60_MODE_OFF, "Выключить", 0xE7E8, QColor("#3B3B3B"), "");
        addTile(AW60_MODE_HARDWARE, "Аппаратный", 0xE713, QColor("#107C10"), "");
        auto gridHolder = new QWidget;
        gridHolder->setObjectName("flat");
        gridHolder->setLayout(grid);
        left->addWidget(gridHolder);
        left->addWidget(effectHint);
        cols->addLayout(left, 1);

        QVBoxLayout* inner;
        auto side = panel(inner);
        side->setFixedWidth(300);
        inner->addWidget(heading("Параметры"));
        brightness->setRange(0, 255); brightness->setValue(255);
        speed->setRange(1, 100); speed->setValue(35);
        fpsLimit->setRange(1, 60); fpsLimit->setValue(60);
        effectScale->setRange(1, 100); effectScale->setValue(50);
        effectAmount->setRange(1, 100); effectAmount->setValue(60);
        effectBox = new QWidget;
        effectBox->setObjectName("flat");
        auto eb = new QVBoxLayout(effectBox);
        eb->setContentsMargins(0, 0, 0, 0);
        eb->setSpacing(10);
        eb->addWidget(sliderRow("Яркость", brightness, [](int v) { return QString("%1%").arg(v * 100 / 255); }));
        eb->addWidget(sliderRow("Скорость", speed, [](int v) { return QString::number(v); }));
        eb->addWidget(sliderRow("Частота кадров", fpsLimit, [](int v) { return QString("%1 FPS").arg(v); }));
        eb->addWidget(sliderRow("Размер", effectScale, [](int v) { return QString::number(v); }));
        eb->addWidget(sliderRow("Глубина / фон", effectAmount, [](int v) { return QString::number(v); }));
        inner->addWidget(effectBox);

        hardware->addItem("Ripple", 5); hardware->addItem("Static", 1); hardware->addItem("Wave", 4);
        hardware->addItem("Reactive", 8); hardware->addItem("Rain", 6); hardware->addItem("Spectrum", 3);
        hardwareRow = row({text("Штатный эффект", 10, kSub), hardware});
        inner->addWidget(hardwareRow);
        connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateModeUi(); });

        auto pick = [this](QColor* target, const QString& title, QPushButton* b) {
            connect(b, &QPushButton::clicked, this, [this, target, title] {
                auto c = QColorDialog::getColor(*target, this, title);
                if (c.isValid()) { *target = c; updateColorButtons(); }
            });
        };
        pick(&color, "Основной цвет", pickA);
        pick(&color2, "Второй цвет", pickB);
        pick(&color3, "Третий цвет / блик", pickC);
        updateColorButtons();
        inner->addWidget(text("Цвета палитры", 10, kSub));
        inner->addWidget(row({pickA, pickB, pickC}, 6));
        inner->addSpacing(4);
        inner->addWidget(button("Включить", [this] { apply(); }, true));
        inner->addWidget(button("Окрасить выбранные", [this] {
            if (lighting->selected.isEmpty()) { notice("Выберите клавиши на схеме", true); return; }
            for (int i : lighting->selected) lighting->colors[i] = color;
            lighting->update();
            selectMode(AW60_MODE_DIRECT);
        }));
        inner->addWidget(button("Окрасить все", [this] { lighting->colors.fill(color); lighting->update(); selectMode(AW60_MODE_DIRECT); }));
        inner->addWidget(button("Пауза", [this] { post([this] { device->pause(); }); }));
        inner->addStretch();
        cols->addWidget(side);
        outer->addLayout(cols, 1);
    }
    void buildKeys() {
        auto l = page("Клавиши", "Переназначения хранятся в клавиатуре и работают без приложения. Fn остаётся доступной. Макро-слоты переиспользуются под световые пресеты.");
        l->addWidget(mapping, 1);
        mapping->changed = [this] { selectedAction(); };
        l->addWidget(actionInfo);
        layer->addItems({"Основной слой", "Fn-слой"});
        connect(layer, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { selectedAction(); });
        actionType->addItems({"Клавиша", "Мультимедиа", "Отключить"});
        connect(actionType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { targets(); });
        targets();
        l->addWidget(row({layer, actionType, target}));
        l->addWidget(button("Записать выбранные назначения в клавиатуру", [this] { applyKeys(); }, true));
    }
    void buildPresets() {
        auto l = page("Пресеты", "Слоты 0..49 сохраняются в память клавиатуры вместо макросов. Штатная смена подсветки на клавиатуре листает все валидные слоты по порядку, после последнего показывает штатный эффект и начинает заново.");
        presetSlot->setRange(0, 49);
        presetSlot->setPrefix("Слот ");
        presetNameEdit->setPlaceholderText("Имя пресета (необязательно)");
        QVBoxLayout* inner;
        auto box = panel(inner);
        inner->addWidget(heading("Слот пресета"));
        inner->addWidget(row({presetSlot, presetNameEdit}));
        inner->addWidget(row({button("Прочитать слот", [this] { int slot = presetSlot->value(); post([this, slot] { device->readPreset(slot); }); }),
            button("Записать текущий эффект", [this] {
                if (!isEffectMode(currentMode())) { notice("В слот можно сохранить только автономный эффект", true); return; }
                QJsonObject r{{"slot", presetSlot->value()}, {"raw", QString(encodePreset().toHex())}};
                post([this, r] { device->writePreset(r); });
                notice("Запись пресета…");
            }, true),
            button("Очистить слот", [this] {
                QJsonObject r{{"slot", presetSlot->value()}, {"raw", QString(QByteArray(AW60_PRESET_SLOT_BYTES, '\0').toHex())}};
                post([this, r] { device->writePreset(r); });
                notice("Очистка слота…");
            })}));
        l->addWidget(box);
        QVBoxLayout* slotsInner;
        auto slotsBox = panel(slotsInner);
        slotsInner->addWidget(heading("Все слоты"));
        slotsInner->addWidget(text("Нажмите «Прочитать все слоты», затем на слот, чтобы загрузить его в редактор. Пустые слоты серые.", 9));
        auto slotGrid = new QGridLayout;
        slotGrid->setSpacing(6);
        for (int i = 0; i < AW60_PRESET_SLOT_COUNT; ++i) {
            auto t = new Tile(QString("%1 · пусто").arg(i), 0xE734, QColor("#3B3B3B"));
            t->setCheckable(false);
            slotTiles.append(t);
            connect(t, &QAbstractButton::clicked, this, [this, i] {
                presetSlot->setValue(i);
                if (validPreset(slotRaw[i])) applyPresetBlob(slotRaw[i], true);
                else notice(QString("Слот %1 пуст или ещё не прочитан").arg(i), slotRaw[i].isEmpty());
            });
            slotGrid->addWidget(t, i / 10, i % 10);
        }
        auto slotHolder = new QWidget;
        slotHolder->setObjectName("flat");
        slotHolder->setLayout(slotGrid);
        slotsInner->addWidget(slotHolder);
        slotsInner->addWidget(button("Прочитать все слоты", [this] { notice("Чтение слотов…"); post([this] { device->scanPresets(); }); }));
        l->addWidget(slotsBox);
        l->addWidget(text("В слот записываются тип эффекта, скорость, яркость, FPS, размер, глубина и три цвета. Чтобы применить прочитанный слот, вернитесь на страницу «Подсветка» и нажмите «Включить»."));
        l->addStretch();
    }
    void buildResponse() {
        auto l = page("Отклик", "Интервал USB, сканирование матрицы и debounce — разные величины. Здесь отображаются подтверждённые параметры.");
        QVBoxLayout* inner;
        auto usbBox = panel(inner);
        inner->addWidget(heading("USB"));
        inner->addWidget(usbResult);
        inner->addWidget(button("Проверить скорость USB и интервалы endpoint", [this] {
            auto p = new QProcess(this);
            connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, p](int code, QProcess::ExitStatus) {
                auto a = QJsonDocument::fromJson(p->readAllStandardOutput()).array();
                QString s;
                for (auto v : a) {
                    auto o = v.toObject();
                    s += QString("VID 3151 / PID %1 • %2\n").arg(o["pid"].toInt(), 4, 16, QChar('0')).arg(o["speed"].toInt() == 1 ? "USB Full Speed" : "USB speed code " + QString::number(o["speed"].toInt()));
                    for (auto e : o["endpoints"].toArray()) {
                        auto ep = e.toObject();
                        if (ep["type"].toInt() == 3)
                            s += QString("Endpoint 0x%1: интервал %2 мс • до %3 Гц\n").arg(ep["address"].toInt(), 2, 16, QChar('0')).arg(ep["interval"].toInt()).arg(1000. / qMax(1, ep["interval"].toInt()));
                    }
                }
                usbResult->setText(code == 0 && !s.isEmpty() ? s + "\nЭто данные USB-дескриптора, а не измерение полной задержки клавиши. Для Full Speed предел interrupt-опроса — 1000 Гц." : "Не удалось прочитать USB. Подключите устройство и повторите.");
                p->deleteLater();
            });
            p->start(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("usb_rates.exe"));
        }));
        debounce->setRange(1, 10);
        debounce->setValue(1);
        debounce->setSuffix(" мс");
        inner->addWidget(row({text("Debounce", 10, kText), debounce, button("Применить debounce", [this] {
            QJsonObject r{{"kind", "debounce"}, {"value", debounce->value()}};
            post([this, r] { device->setting(r); });
        })}));
        inner->addWidget(text("1 мс — минимальная штатная настройка. При дребезге увеличьте значение. Более высокая частота USB требует поддержки High Speed в контроллере; один ползунок её не добавит."));
        l->addWidget(usbBox);

        QVBoxLayout* mon;
        auto monBox = panel(mon);
        mon->addWidget(heading("Монитор нажатий"));
        mon->addWidget(text("Показывает, какие клавиши прошивка видит прямо сейчас — те же сигналы получают эффекты с откликом. Работает по USB-кабелю, нужна прошивка rev6."));
        monitor->setMinimumHeight(200);
        mon->addWidget(monitor);
        monitorToggle = button("Включить монитор", [this] {
            bool on = monitorToggle->property("on").toBool();
            monitorToggle->setProperty("on", !on);
            monitorToggle->setText(on ? "Включить монитор" : "Выключить монитор");
            monitor->pressed.clear();
            monitor->update();
            monitorInfo->setText(on ? "Монитор выключен" : "Ожидание данных…");
            post([this, on] { device->keyMonitor(!on); });
        }, true);
        mon->addWidget(row({monitorToggle, monitorInfo}));
        l->addWidget(monBox);

        auto input = new QTextEdit;
        input->setPlaceholderText("Проверка набора текста. Этот текст не записывается в профиль.");
        input->setMinimumHeight(90);
        l->addWidget(input);
        l->addStretch();
    }
    void buildProfiles() {
        auto l = page("Профили", "Локальный профиль хранит текущую страницу подсветки и редактор пресета. Аппаратный профиль выбирается отдельно.");
        QVBoxLayout* inner;
        auto box = panel(inner);
        inner->addWidget(heading("Файл профиля"));
        inner->addWidget(row({button("Сохранить профиль…", [this] {
            QString p = QFileDialog::getSaveFileName(this, "Сохранить профиль", QDir::homePath() + "/Warlock-profile.json", "Warlock profile (*.json)");
            if (!p.isEmpty()) save(p);
        }), button("Загрузить профиль…", [this] {
            QString p = QFileDialog::getOpenFileName(this, "Загрузить профиль", QDir::homePath(), "Warlock profile (*.json)");
            if (!p.isEmpty()) load(p);
        })}));
        l->addWidget(box);
        QVBoxLayout* hw;
        auto hwBox = panel(hw);
        hw->addWidget(heading("Аппаратный профиль"));
        nativeProfile->setRange(0, 4);
        nativeProfile->setPrefix("Профиль № ");
        hw->addWidget(row({nativeProfile, button("Переключить аппаратный профиль", [this] {
            QJsonObject r{{"kind", "profile"}, {"value", nativeProfile->value()}};
            post([this, r] { device->setting(r); });
        }, true)}));
        l->addWidget(hwBox);
        l->addWidget(row({button("Восстановить слой / пресет из копии…", [this] { restoreBackup(); }),
            button("Открыть резервные копии", [this] {
                QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
                QDir().mkpath(dir);
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            })}));
        l->addStretch();
    }
    void buildDevice() {
        auto l = page("Устройство", "Прямой доступ к клавиатуре. OpenRGB не требуется для работы Studio.");
        QVBoxLayout* inner;
        auto box = panel(inner);
        inner->addWidget(heading("Подключение"));
        inner->addWidget(row({button("USB / Авто", [this] { post([this] { device->open(false); }); }, true),
            button("Свисток 2.4 GHz", [this] { post([this] { device->open(true); }); }),
            button("Статистика / батарея", [this] { post([this] { device->refresh(); }); })}));
        inner->addWidget(text("Частота кадров", 10, kSub));
        inner->addWidget(fps);
        l->addWidget(box);

        QVBoxLayout* win;
        auto winBox = panel(win);
        win->addWidget(heading("Клавиатура"));
        auto winlock = new QCheckBox("Блокировать Windows-клавишу");
        win->addWidget(winlock);
        win->addWidget(button("Применить блокировку Win", [this, winlock] {
            QJsonObject r{{"kind", "winlock"}, {"value", winlock->isChecked() ? 1 : 0}};
            post([this, r] { device->setting(r); });
        }));
        l->addWidget(winBox);

        QVBoxLayout* look;
        auto lookBox = panel(look);
        look->addWidget(heading("Цвет акцента"));
        auto swatches = new QHBoxLayout;
        swatches->setSpacing(6);
        for (auto code : kAccents) {
            auto b = new QPushButton;
            b->setFixedSize(40, 40);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QString("QPushButton{background:%1;border:2px solid transparent;padding:0;min-height:0;}QPushButton:hover{border-color:#FFFFFF;}").arg(code));
            QColor c(code);
            connect(b, &QPushButton::clicked, this, [this, c] { setAccent(c); });
            swatches->addWidget(b);
        }
        swatches->addStretch();
        look->addLayout(swatches);
        l->addWidget(lookBox);
        QVBoxLayout* diag;
        auto diagBox = panel(diag);
        diag->addWidget(heading("Диагностика прошивки"));
        diag->addWidget(text("Замеры самой клавиатуры (rev8): паузы главного цикла, загрузка процессора прерываниями, время отрисовки кадров. Работает по USB-кабелю.", 9));
        diag->addWidget(row({button("Сбросить счётчики", [this] { post([this] { device->perfReport(true); }); }),
            button("Снять замеры", [this] { post([this] { device->perfReport(false); }); }, true)}));
        diag->addWidget(perfOut);
        l->addWidget(diagBox);
        l->addWidget(text("Резервная копия слоёв создаётся при подключении и перед записью. Слоты пресетов проверяются чтением после сохранения. При использовании Studio поставьте анимации OpenRGB на паузу, чтобы приложения не спорили за подсветку."));
        l->addStretch();
    }

public:
    bool selfTest() {
        QTemporaryDir dir;
        if (!dir.isValid()) return false;
        selectMode(AW60_MODE_FIRST_EFFECT + 15);
        brightness->setValue(137); speed->setValue(43); fpsLimit->setValue(60); effectScale->setValue(61); effectAmount->setValue(74);
        color = QColor("#ff8800"); color2 = QColor("#ff2ea6"); color3 = QColor("#7ffff0");
        presetSlot->setValue(7);
        presetNameEdit->setText("Key Ripple Slot");
        lighting->colors[0] = QColor("#ff8800");
        updateColorButtons();
        monitor->pressed = {44, 45, 74};
        auto before = state();
        auto slotBefore = presetSlot->value();
        auto nameBefore = presetNameEdit->text();
        save(dir.path() + "/roundtrip.json");
        selectMode(AW60_MODE_DIRECT);
        presetSlot->setValue(0);
        presetNameEdit->clear();
        load(dir.path() + "/roundtrip.json");
        bool ok = state() == before && presetSlot->value() == slotBefore && presetNameEdit->text() == nameBefore;
        // The preset blob must carry the new effect and the explicit-palette flag.
        auto blob = encodePreset();
        ok = ok && validPreset(blob) && quint8(blob[AW60_PRESET_MODE_OFFSET]) == 15 && (quint8(blob[AW60_PRESET_FLAGS_OFFSET]) & AW60_FLAG_EXPLICIT_PALETTE);
        for (int i = 0; i < pages->count(); ++i) {
            setPage(i);
            qApp->processEvents();
            grab().save(QCoreApplication::applicationDirPath() + QString("/studio-page-%1.png").arg(i));
        }
        setPage(0);
        return ok;
    }
    void setPage(int i) {
        pages->setCurrentIndex(i);
        if (i >= 0 && i < navButtons.size()) navButtons[i]->setChecked(true);
    }
    explicit Window(bool radio = false) : preferRadio(radio), settings("Warlock", "WarlockStudio") {
        setWindowTitle("WARLOCK / Studio");
        setMinimumSize(1280, 740);
        resize(1360, 900);
        setFont(uiFont(10));
        accent = QColor(settings.value("accent", "#0078D7").toString());
        if (!accent.isValid()) accent = QColor("#0078D7");

        auto central = new QWidget;
        setCentralWidget(central);
        auto outer = new QVBoxLayout(central);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        auto body = new QHBoxLayout;
        body->setSpacing(0);
        outer->addLayout(body, 1);
        outer->addWidget(status);

        auto sidebar = new QWidget;
        sidebar->setFixedWidth(232);
        sidebar->setObjectName("sidebar");
        sidebar->setStyleSheet("#sidebar{background:#171717;}");
        auto side = new QVBoxLayout(sidebar);
        side->setContentsMargins(0, 22, 0, 16);
        side->setSpacing(0);
        auto brand = new QVBoxLayout;
        brand->setContentsMargins(20, 0, 16, 18);
        brand->setSpacing(0);
        brand->addWidget(text("WARLOCK", 24, kText, "Segoe UI Light"));
        brand->addWidget(text("STUDIO  •  2.1", 8.5, accent, "Segoe UI Semibold"));
        side->addLayout(brand);
        struct NavItem { const char* label; ushort icon; };
        const NavItem items[] = {{"Подсветка", 0xE706}, {"Клавиши", 0xE765}, {"Пресеты", 0xE734}, {"Отклик", 0xE7C9}, {"Профили", 0xE77B}, {"Устройство", 0xE772}};
        int index = 0;
        for (auto item : items) {
            auto b = new NavButton(QString::fromUtf8(item.label), item.icon);
            b->accent = accent;
            navGroup->addButton(b, index);
            navButtons.append(b);
            side->addWidget(b);
            ++index;
        }
        side->addStretch(1);
        auto linkBox = new QVBoxLayout;
        linkBox->setContentsMargins(20, 0, 16, 0);
        linkBox->addWidget(link);
        side->addLayout(linkBox);
        connect(navGroup, &QButtonGroup::idClicked, this, [this](int id) { pages->setCurrentIndex(id); });
        body->addWidget(sidebar);
        body->addWidget(pages, 1);

        buildLighting();
        buildKeys();
        buildPresets();
        buildResponse();
        buildProfiles();
        buildDevice();
        for (auto k : {lighting, mapping, monitor}) k->accent = accent;
        const bool rendering = qApp->arguments().contains("--self-test") || qApp->arguments().contains("--screenshot");
        if (!rendering) restoreGeometry(settings.value("geometry").toByteArray());
        auto shortcut = [this](const QString& keys, std::function<void()> f) {
            auto sc = new QShortcut(QKeySequence(keys), this);
            connect(sc, &QShortcut::activated, this, [f] { f(); });
        };
        for (int i = 0; i < 6; ++i) shortcut(QString("Ctrl+%1").arg(i + 1), [this, i] { setPage(i); });
        shortcut("Ctrl+Return", [this] { apply(); });
        shortcut("Ctrl+A", [this] {
            if (pages->currentIndex() != 0) return;
            for (int i = 0; i < 80; ++i) lighting->selected.insert(i);
            lighting->update();
            selection->setText("Выбрано клавиш: 80");
        });
        shortcut("Esc", [this] { lighting->selected.clear(); lighting->update(); selection->setText("Выберите клавиши на схеме"); });
        shortcut("F5", [this] { notice("Переподключение…"); post([this] { device->open(preferRadio); }); });
        setPage(rendering ? 0 : qBound(0, settings.value("page", 0).toInt(), 5));
        selectMode(AW60_MODE_DIRECT);
        updateColorButtons();
        notice("Подключение…");
        setLink(false, "Ищем клавиатуру…");

        device->moveToThread(&usb);
        connect(&usb, &QThread::finished, device, &QObject::deleteLater);
        connect(device, &Device::message, this, [this](QString s, bool e) { notice(s, e); });
        connect(device, &Device::linkLost, this, [this] { setLink(false, "Связь потеряна.\nF5 — переподключить"); });
        connect(device, &Device::connected, this, [this](QJsonObject data) {
            for (auto i = data.begin(); i != data.end(); ++i) info[i.key()] = i.value();
            setLink(true, QString("%1 • прошивка AW60 rev %2\nАппаратный профиль %3")
                              .arg(info["radio"].toBool() ? "2.4 GHz" : "USB-кабель")
                              .arg(info["revision"].toInt()).arg(info["profile"].toInt()));
            debounce->setValue(info["debounce"].toInt(1));
            nativeProfile->setValue(info["profile"].toInt());
            if (data.contains("effect")) {
                selectMode(data["effect"].toInt() + AW60_MODE_FIRST_EFFECT);
                speed->setValue(data["effectSpeed"].toInt());
                brightness->setValue(data["effectBrightness"].toInt());
                fpsLimit->setValue(data["effectFps"].toInt());
                effectScale->setValue(data["effectScale"].toInt(50));
                effectAmount->setValue(data["effectAmount"].toInt(60));
                color = QColor(data["effectColor"].toString());
                color2 = QColor(data["effectColor2"].toString("#ff4fd8"));
                color3 = QColor(data["effectColor3"].toString("#7bffef"));
                if (data.contains("presetSlot")) presetSlot->setValue(data["presetSlot"].toInt());
            }
            selectedAction();
            updateColorButtons();
            updateModeUi();
            if (qApp->arguments().contains("--demo") && !property("demoStarted").toBool()) {
                setProperty("demoStarted", true);
                selectMode(AW60_MODE_FIRST_EFFECT);
                QTimer::singleShot(0, this, [this] { apply(); });
            }
        });
        connect(device, &Device::presetLoaded, this, [this](QJsonObject p) {
            presetSlot->setValue(p["slot"].toInt());
            applyPresetBlob(QByteArray::fromHex(p["raw"].toString().toLatin1()), true);
        });
        connect(device, &Device::presetScanned, this, [this](int slot, QString raw, bool) {
            auto blob = QByteArray::fromHex(raw.toLatin1());
            slotRaw[slot] = blob;
            auto t = slotTiles[slot];
            if (validPreset(blob)) {
                auto at = [&](int i) { return int(quint8(blob[i])); };
                QColor c(at(AW60_PRESET_COLOR1_OFFSET), at(AW60_PRESET_COLOR1_OFFSET + 1), at(AW60_PRESET_COLOR1_OFFSET + 2));
                if (c.lightness() < 90) c = c.lighter(190 - c.lightness());
                t->tint = c.darker(120);
                QString name = presetName(blob);
                t->setText(QString("%1 · %2").arg(slot).arg(name.isEmpty() ? QString::fromUtf8(kEffects[quint8(blob[AW60_PRESET_MODE_OFFSET])].label) : name));
            } else {
                t->tint = QColor("#3B3B3B");
                t->setText(QString("%1 · пусто").arg(slot));
            }
            t->update();
        });
        connect(device, &Device::perfData, this, [this](QJsonObject r) { showPerf(r); });
        connect(device, &Device::keyState, this, [this](QJsonObject d) {
            auto held = QByteArray::fromHex(d["held"].toString().toLatin1());
            QSet<int> now;
            for (int k = 0; k < 80 && k / 8 < held.size(); ++k)
                if (quint8(held[k / 8]) & (1 << (k % 8))) now.insert(k);
            if (now != monitor->pressed) { monitor->pressed = now; monitor->update(); }
            monitorInfo->setText(QString("Нажатий: %1 • режим прошивки: %2 • матрица: %3").arg(d["presses"].toInt()).arg(d["tracking"].toBool() ? "слежение активно" : "включите эффект с откликом").arg(d["raw"].toString()));
        });
        connect(device, &Device::telemetry, this, [this](QJsonObject d) {
            if (d.contains("fps")) fps->setText(QString::number(d["fps"].toDouble(), 'f', 1) + " FPS");
            if (d.contains("frames")) notice(QString("Кадров принято: %1 • ошибок: %2").arg(d["frames"].toInt()).arg(d["errors"].toInt()));
            if (d.contains("battery")) notice(QString("Батарея: %1% (данные приёмника)").arg(d["battery"].toInt()));
        });
        usb.start();
        if (!qApp->arguments().contains("--self-test"))
            QTimer::singleShot(100, this, [this] { post([this] { device->open(preferRadio); }); });
    }
    void closeEvent(QCloseEvent* e) override {
        settings.setValue("geometry", saveGeometry());
        settings.setValue("page", pages->currentIndex());
        QMainWindow::closeEvent(e);
    }
    ~Window() override {
        QMetaObject::invokeMethod(device, &Device::shutdown, Qt::BlockingQueuedConnection);
        usb.quit();
        usb.wait();
    }
};

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication a(argc, argv);
    a.setApplicationName("WarlockStudio");
    a.setOrganizationName("Warlock");
    if (a.arguments().contains("--screenshot") || a.arguments().contains("--self-test")) {
        const QString fonts = QString::fromLocal8Bit(qgetenv("WINDIR")) + "/Fonts/";
        for (auto f : {"segoeui.ttf", "segoeuil.ttf", "segoeuisl.ttf", "seguisb.ttf", "segmdl2.ttf"}) QFontDatabase::addApplicationFont(fonts + f);
    }
    a.setFont(uiFont(10));
    a.setProperty("accent", QSettings("Warlock", "WarlockStudio").value("accent", "#0078D7").toString());
    QSettings prefs("Warlock", "WarlockStudio");
    QColor accent(prefs.value("accent", "#0078D7").toString());
    a.setStyleSheet(metroStyleSheet(accent.isValid() ? accent : QColor("#0078D7")));
    Window w(a.arguments().contains("--radio"));
    w.show();
    if (a.arguments().contains("--self-test")) {
        QTimer::singleShot(100, &a, [&] {
            bool ok = w.selfTest();
            qInfo() << "Profile roundtrip and six page renders:" << ok;
            a.exit(ok ? 0 : 1);
        });
    }
    if (a.arguments().contains("--screenshot")) {
        QTimer::singleShot(2000, &a, [&] {
            w.grab().save(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("studio-preview.png"));
            a.quit();
        });
    }
    return a.exec();
}
