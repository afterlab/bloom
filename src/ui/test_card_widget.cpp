#include <bloom/ui/test_card_widget.hpp>

#include "test_card_expectation.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/project_view_memory.hpp>

#include <QColor>
#include <QCoreApplication>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <variant>

namespace bloom::ui {

struct TestCardCardWidget::State final {
    std::int64_t card = -1;
    std::int64_t size = 0;
    bool step = false;
    std::int64_t value = 0;
    std::int64_t space = 0;
    // Identifies the request whose answer is current.
    quint64 generation = 0;
    QString requested;
};

namespace {

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] int swatchSide() { return kit::px(kit::Size::Control) * 2; }

[[nodiscard]] std::optional<document::ParameterValue>
valueOf(const CompositionSession& session, const document::NodeId node, const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto* composition = session.composition();
    if (!id || composition == nullptr)
        return std::nullopt;
    if (const auto live = session.liveValue(*id))
        return live;
    const auto* record = composition->parameters().find(*id);
    if (record == nullptr)
        return std::nullopt;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
        return constant->value;
    return std::nullopt;
}

[[nodiscard]] std::int64_t integerOf(const CompositionSession& session, const document::NodeId node,
                                     const std::string_view role) {
    const auto value = valueOf(session, node, role);
    if (const auto* integer = value ? std::get_if<std::int64_t>(&*value) : nullptr)
        return *integer;
    const auto* spec = document::testCardParameterSpecForRole(role);
    return spec != nullptr ? static_cast<std::int64_t>(spec->value) : 0;
}

[[nodiscard]] bool booleanOf(const CompositionSession& session, const document::NodeId node,
                             const std::string_view role) {
    const auto value = valueOf(session, node, role);
    if (const auto* flag = value ? std::get_if<bool>(&*value) : nullptr)
        return *flag;
    const auto* spec = document::testCardParameterSpecForRole(role);
    return spec != nullptr && spec->value != 0.0;
}

[[nodiscard]] QString signalLine(const std::array<double, 3>& rgb, const QString& space) {
    return QCoreApplication::translate("TestCard", "Signal %1 %2 %3 in %4")
        .arg(rgb[0], 0, 'f', 3)
        .arg(rgb[1], 0, 'f', 3)
        .arg(rgb[2], 0, 'f', 3)
        .arg(space);
}

} // namespace

TestCardCardWidget::TestCardCardWidget(CompositionSession& session, const document::NodeId node,
                                       const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node), state_(std::make_unique<State>()) {
    setObjectName(card ? QStringLiteral("nodeTestCard") : QStringLiteral("propertiesTestCard"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    signal_ = new kit::KLabel(QString{}, this);
    signal_->setObjectName(QStringLiteral("testCardSignal"));
    signal_->setWordWrap(true);
    expected_ = new kit::KLabel(QString{}, this);
    expected_->setObjectName(QStringLiteral("testCardExpected"));
    expected_->setWordWrap(true);
    note_ = new kit::KLabel(
        tr("Verifies composition, Viewer and monitor, not a display calibration. A mismatch can "
           "come from Bloom, the desktop compositor, the GPU output settings or the monitor's "
           "profile."),
        this);
    note_->setObjectName(QStringLiteral("testCardNote"));
    note_->setWordWrap(true);
    note_->setTone(kit::Color::Muted);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::evaluationChanged})
        connect(&session_, signal, this, &TestCardCardWidget::refresh);
    refresh();
}

TestCardCardWidget::~TestCardCardWidget() { ++state_->generation; }

bool TestCardCardWidget::showsRole(const std::string_view role) const {
    return document::testCardShowsRole(state_->card, state_->size, state_->step, role);
}

QString TestCardCardWidget::signalText() const { return signal_->text(); }
QString TestCardCardWidget::expectedText() const { return expected_->text(); }

int TestCardCardWidget::heightAt(const int width) const {
    if (!patch_)
        return 0;
    const int text = std::max(0, width - swatchSide() - small());
    const auto wrapped = [text](const kit::KLabel* label) {
        return label->hasHeightForWidth() ? label->heightForWidth(std::max(text, 1))
                                          : label->sizeHint().height();
    };
    const int block = wrapped(signal_) + small() + wrapped(expected_);
    return std::max(swatchSide(), block) + small() +
           (note_->hasHeightForWidth() ? note_->heightForWidth(std::max(width, 1))
                                       : note_->sizeHint().height());
}

int TestCardCardWidget::minimumWidthHint() const {
    return kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
}

QSize TestCardCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void TestCardCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void TestCardCardWidget::place() {
    signal_->setVisible(patch_);
    expected_->setVisible(patch_);
    note_->setVisible(patch_);
    if (!patch_)
        return;
    const int x = swatchSide() + small();
    const int text = std::max(1, width() - x);
    const int signalHeight = signal_->heightForWidth(text);
    const int expectedHeight = expected_->heightForWidth(text);
    signal_->setGeometry(x, 0, text, signalHeight);
    expected_->setGeometry(x, signalHeight + small(), text, expectedHeight);
    const int block = std::max(swatchSide(), signalHeight + small() + expectedHeight);
    note_->setGeometry(0, block + small(), width(), note_->heightForWidth(width()));
}

void TestCardCardWidget::paintEvent(QPaintEvent* event) {
    NodeCardWidget::paintEvent(event);
    if (!patch_ || !swatch_.isValid())
        return;
    QPainter painter(this);
    painter.fillRect(QRect(0, 0, swatchSide(), swatchSide()), swatch_);
}

void TestCardCardWidget::expectationReady(const quint64 generation, QString expected,
                                          QColor swatch) {
    if (generation != state_->generation)
        return;
    expected_->setText(expected);
    swatch_ = swatch;
    update();
    emit rowsChanged();
}

void TestCardCardWidget::refresh() {
    const auto card = integerOf(session_, node_, "card");
    const auto size = integerOf(session_, node_, "sizeMode");
    const bool step = booleanOf(session_, node_, "patchStep");
    const auto value = integerOf(session_, node_, "patchValue");
    const auto space = integerOf(session_, node_, "patchSpace");
    const bool changed = card != state_->card || size != state_->size || step != state_->step;
    state_->card = card;
    state_->size = size;
    state_->step = step;
    state_->value = value;
    state_->space = space;
    patch_ = card == static_cast<std::int64_t>(document::TestCard::Patch);
    if (patch_) {
        const auto signal = document::testCardPatchSignal(step ? 0 : value);
        const auto spaceId = std::string(document::testCardSignalSpaceId(card, space, 0));
        const auto spaceName =
            QString::fromUtf8(document::test_card_parameters::kPatchSpaces[static_cast<std::size_t>(
                std::clamp<std::int64_t>(space, 0, 3))]);
        if (step) {
            signal_->setText(tr("Steps through the whole set, one patch per frame."));
            expected_->setText(
                tr("Each frame's expected display value is its own patch's value in %1.")
                    .arg(spaceName));
            swatch_ = QColor();
        } else {
            signal_->setText(signalLine(signal, spaceName));
            const auto settings = session_.colorSettings();
            const auto* builtIn =
                std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
            const auto digest = settings.ocioConfig.expectedRevision.digest.toLowercaseHex();
            const auto revision = QString::fromLatin1(digest.data(), static_cast<int>(digest.size()));
            const auto remembered = session_.projectViewMemory().recallDisplayView(revision);
            const auto working = std::string(session_.colorIntent().workingColorSpaceId);
            const auto key = QStringLiteral("%1|%2|%3|%4|%5|%6")
                                 .arg(revision, QString::fromStdString(working))
                                 .arg(value)
                                 .arg(space)
                                 .arg(remembered ? remembered->join('/') : QString{}, QString::number(card));
            if (builtIn == nullptr) {
                expected_->setText(tr("Expected display value unavailable for this colour "
                                      "configuration."));
                swatch_ = QColor();
            } else if (key != state_->requested) {
                state_->requested = key;
                expected_->setText(tr("Expected display value: working it out…"));
                const auto generation = ++state_->generation;
                QPointer<TestCardCardWidget> self(this);
                const auto uri = builtIn->uri;
                const auto expectedRevision = settings.ocioConfig.expectedRevision.digest;
                QThreadPool::globalInstance()->start([self, generation, uri, expectedRevision,
                                                      working, remembered, spaceId, signal] {
                    QString text;
                    QColor swatch;
                    auto resolution = color::resolveOcioBuiltIn(
                        color::OcioConfigLocatorKind::BloomBuiltIn, uri, expectedRevision, working);
                    auto resolved = std::move(resolution).takeResolved();
                    if (resolved && !resolved->displays().empty()) {
                        std::string display;
                        std::string view;
                        if (remembered && remembered->size() == 2) {
                            display = remembered->at(0).toStdString();
                            view = remembered->at(1).toStdString();
                        }
                        const bool known = std::ranges::any_of(
                            resolved->displays(), [&](const auto& entry) {
                                return entry.display == display && entry.view == view;
                            });
                        if (!known) {
                            const auto* pair = color::viewerDefaultDisplayView(resolved->displays());
                            display = pair != nullptr ? pair->display : std::string{};
                            view = pair != nullptr ? pair->view : std::string{};
                        }
                        const auto expectation =
                            patchExpectation(*resolved, display, view, spaceId, signal);
                        if (expectation) {
                            text = QCoreApplication::translate(
                                       "TestCard",
                                       "Expected on %1: 8-bit %2 %3 %4, float %5 %6 %7")
                                       .arg(QString::fromUtf8(color::displayViewLabel(view)))
                                       .arg(expectation->code[0])
                                       .arg(expectation->code[1])
                                       .arg(expectation->code[2])
                                       .arg(expectation->display[0], 0, 'f', 4)
                                       .arg(expectation->display[1], 0, 'f', 4)
                                       .arg(expectation->display[2], 0, 'f', 4);
                            swatch = QColor(expectation->code[0], expectation->code[1],
                                            expectation->code[2]);
                        }
                    }
                    if (text.isEmpty())
                        text = QCoreApplication::translate(
                            "TestCard", "Expected display value unavailable.");
                    if (self)
                        QMetaObject::invokeMethod(
                            self.data(),
                            [self, generation, text, swatch] {
                                if (self)
                                    self->expectationReady(generation, text, swatch);
                            },
                            Qt::QueuedConnection);
                });
            }
        }
    } else {
        state_->requested.clear();
    }
    place();
    if (changed)
        emit rowsChanged();
}

} // namespace bloom::ui
