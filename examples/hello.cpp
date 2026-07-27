// Placeholder example. Once the framework has a server, this becomes the
// canonical "hello world" route.

#include <carafe/version.hpp>

#include <iostream>

int main() {
    std::cout << "carafe " << carafe::version() << '\n';
    return 0;
}
