// A small libhike program: two panes, a focus ring, and a key to quit.
//
// Build with no include or library flags once ardio is installed:
//
//     clang++ -std=c++20 hello.cpp -lhike -o hello
//
#include <hike/hike.hpp>
#include <cstdio>
#include <string>

int main() {
    try {
        // The context puts the terminal in raw mode and restores it in its
        // destructor, so an early return or an exception cannot leave the
        // shell unusable.
        hike::Context ctx;

        hike::Focus focus;
        int presses = 0;
        std::string status = "tab moves focus, enter presses, q quits";

        while (true) {
            ctx.clear();

            // Children are values, so a layout contains them rather than
            // taking a parallel list of sizes.
            auto ui = hike::column(
                hike::fixed(3, hike::box("libhike").child(
                    hike::label("hello from a terminal"))),
                hike::weight(1, hike::row(
                    hike::weight(1, hike::button("press me")
                                        .on_click([&] { ++presses; })),
                    hike::weight(1, hike::label(
                        "pressed " + std::to_string(presses) + " times"))
                ).gap(1)),
                hike::fixed(1, hike::label(status))
            ).gap(1).pad(1);

            ui.draw(ctx, ctx.bounds(), focus);
            ctx.present();

            if (auto event = ctx.poll()) {
                // Quit is handled here rather than by a widget, because it
                // belongs to the application, not to whatever has focus.
                if (event->kind == HIKE_EVENT_KEY && event->key.ch == 'q') break;
                ui.dispatch(*event, focus);
            }
        }
        return 0;
    } catch (const hike::Error& e) {
        // Refusing when stdout is not a terminal is deliberate: writing escape
        // sequences into a pipe would corrupt whatever is reading it.
        std::fprintf(stderr, "hike: %s\n", e.what());
        return 1;
    }
}
