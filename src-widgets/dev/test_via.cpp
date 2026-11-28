/*
 * test-via — OSC 6522's terminal side (via.h): finding messages in output
 * cut anywhere, the foreground check, attach, frames in parts, events, and
 * the ways back to the cells. Dev tool only. Run: ./build/test-via
 */
#include "via.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static QByteArray osc(const QByteArray &body, bool bel = false)
{
    return "\x1b]6522;" + body + (bel ? QByteArray("\x07") : QByteArray("\x1b\\"));
}

static QByteArray b64(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(QByteArray::OmitTrailingEquals);
}

static const QByteArray kHello = "op=hello:v=1:inst=Inst0001abcd:pid=77:schema=puddle.2:app=form:max=65536";

struct Rig
{
    ViaClient via;
    QList<QByteArray> sent;
    int foreground = 77;
    int checks = 0;
    Rig()
    {
        via.setForegroundCheck([this](int pid, std::function<void(bool)> done) {
            ++checks;
            done(pid == foreground);
        });
        QObject::connect(&via, &ViaClient::send, [this](const QByteArray &b, bool) { sent.append(b); });
    }
};

static void testScanner()
{
    ViaScanner s;
    const QByteArray stream = "text" + osc("a=1") + "more\x1b[1m" + osc("b=2", true) + osc("c=3");
    // Every cut point gives the same three messages.
    for (int cut = 0; cut <= stream.size(); ++cut) {
        ViaScanner t;
        QList<QByteArray> got = t.scan(stream.left(cut));
        got += t.scan(stream.mid(cut));
        CHECK(got == (QList<QByteArray>{"a=1", "b=2", "c=3"}));
    }
    // Another sequence cancels an unfinished one.
    CHECK(s.scan("\x1b]6522;x=1\x1b[0m" + osc("y=2")) == QList<QByteArray>{"y=2"});
    // Other OSCs are not ours.
    CHECK(s.scan("\x1b]2;title\x07\x1b]65221;z\x07").isEmpty());
}

static void testAttachAndFrames()
{
    Rig r;
    // A hello cut in two.
    const QByteArray hello = osc(kHello);
    r.via.feed(hello.left(10));
    r.via.feed(hello.mid(10));
    CHECK(r.checks == 1);
    CHECK(r.sent.size() == 1 && r.sent[0].contains("op=attach:v=1:client=") && r.sent[0].contains(":full=1"));
    CHECK(!r.via.active());  // no window yet
    // Hello again (focus) while attached: no second check.
    r.via.feed(hello);
    CHECK(r.checks == 1 && r.sent.size() == 1);

    // A frame in two parts.
    QJsonObject frame{{"title", "Form"},
                      {"modal", false},
                      {"root", QJsonObject{{"type", "button"}, {"id", "ok"}, {"label", "OK"}}}};
    const QByteArray data = b64(frame);
    const int half = data.size() / 2;
    r.via.feed(osc("op=frame:win=main:seq=1:part=1:parts=2:data=" + data.left(half)));
    CHECK(!r.via.active());
    r.via.feed(osc("op=frame:win=main:seq=1:part=2:parts=2:data=" + data.mid(half)));
    CHECK(r.via.active());
    CHECK(r.via.windows().size() == 1 && r.via.windows()[0]->frame.value("title").toString() == "Form");
    // An old frame is ignored.
    frame["title"] = "Old";
    r.via.feed(osc("op=frame:win=main:seq=1:data=" + b64(frame)));
    CHECK(r.via.windows()[0]->frame.value("title").toString() == "Form");

    // An event.
    r.sent.clear();
    r.via.sendEvent("main", QJsonObject{{"node", "ok"}, {"act", "click"}});
    CHECK(r.sent.size() == 1);
    const QByteArray ev = r.sent[0];
    CHECK(ev.contains(":win=main:ack=1:n=1:data="));
    const QByteArray evData = ev.mid(ev.indexOf("data=") + 5).chopped(2);
    CHECK(QJsonDocument::fromJson(QByteArray::fromBase64(evData)).object().value("act").toString() == "click");
    // A long one comes in parts no longer than the program's max.
    r.sent.clear();
    r.via.sendEvent("main", QJsonObject{{"node", "name"}, {"act", "text"}, {"value", QString(100000, 'y')}});
    const QByteArray all = r.sent.value(0);
    CHECK(all.count("\x1b]6522;") > 1);
    for (const QByteArray &m : all.split('\\'))
        CHECK(m.size() <= 65536);

    // Closing the window.
    r.via.feed(osc("op=close:win=main:seq=2"));
    CHECK(!r.via.active());

    // Bye ends it.
    bool ended = false;
    QObject::connect(&r.via, &ViaClient::ended, [&]() { ended = true; });
    r.via.feed(osc("op=bye:inst=Inst0001abcd"));
    CHECK(ended);
}

static void testRefusals()
{
    Rig r;
    r.foreground = 12;  // someone else's hello
    r.via.feed(osc(kHello));
    CHECK(r.checks == 1 && r.sent.isEmpty());
    // A schema we don't draw, or another version: not even checked.
    r.via.feed(osc("op=hello:v=1:inst=Inst0002abcd:pid=77:schema=other.1"));
    r.via.feed(osc("op=hello:v=2:inst=Inst0002abcd:pid=77:schema=puddle.2"));
    CHECK(r.checks == 1);

    // Back to the cells: detach, and this program's hellos are ignored after.
    r.foreground = 77;
    r.via.feed(osc(kHello));
    CHECK(r.sent.size() == 1);
    r.via.detach();
    CHECK(r.sent.size() == 2 && r.sent[1].contains("op=detach:client="));
    r.via.feed(osc(kHello));
    CHECK(r.sent.size() == 2);
    // A new program (another inst) is welcome.
    r.via.feed(osc("op=hello:v=1:inst=Inst0003abcd:pid=77:schema=puddle.2"));
    CHECK(r.sent.size() == 3);
    // The foreground changing ends it.
    bool ended = false;
    QObject::connect(&r.via, &ViaClient::ended, [&]() { ended = true; });
    r.via.foregroundChanged(99);
    CHECK(ended);
}

// test-via --shot out.png: a ViaView of hello.py's window, drawn offscreen.
static int shot(const char *path)
{
    Rig r;
    r.via.feed(osc(kHello));
    const QJsonObject root = QJsonDocument::fromJson(R"json({"type":"column","gap":1,"children":[
        {"type":"label","text":"Hello, Ada!","style":"accent"},
        {"type":"text_input","id":"name","placeholder":"your name","on_change":"typed"},
        {"type":"checkbox","id":"tea","label":"Tea, not coffee"},
        {"type":"row","gap":1,"children":[
            {"type":"button","id":"wave","label":"Wave (3)","primary":true,"on_click":"wave"},
            {"type":"spacer"},
            {"type":"button","id":"bye","label":"Bye"}]}]})json").object();
    QJsonObject frame{{"title", "Python"}, {"modal", false}, {"root", root},
                      {"state", QJsonObject{{"name", QJsonObject{{"text", "Ada"}}}, {"tea", QJsonObject{{"on", true}}}}}};
    r.via.feed(osc("op=frame:win=hi:seq=1:data=" + b64(frame)));
    ViaView view(&r.via);
    view.setColours(QColor("#1d1f21"), QColor("#c5c8c6"), QColor("#5e9cf5"));
    view.resize(480, 300);
    view.show();
    QCoreApplication::processEvents();
    return view.grab().save(QString::fromLocal8Bit(path)) ? 0 : 1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--shot")
        return shot(argv[2]);
    testScanner();
    testAttachAndFrames();
    testRefusals();
    if (failures) {
        std::fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    std::printf("test-via: all passed\n");
    return 0;
}
