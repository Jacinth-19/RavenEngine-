// UUID generation, parsing, nil handling.
#include "test_framework.h"

#include "raven/core/UUID.h"

using namespace raven;

RAVEN_TEST("uuid: generated values are unique and non-nil") {
    auto a = UUID::Generate();
    auto b = UUID::Generate();
    CHECK(!a.IsNil());
    CHECK(!b.IsNil());
    CHECK(a != b);
}

RAVEN_TEST("uuid: string round-trip") {
    auto original = UUID::Generate();
    const std::string text = original.ToString();
    auto parsed = UUID::Parse(text);
    CHECK(parsed.has_value());
    CHECK(*parsed == original);
}

RAVEN_TEST("uuid: parse rejects garbage") {
    CHECK(!UUID::Parse("").has_value());
    CHECK(!UUID::Parse("not-a-uuid").has_value());
    CHECK(!UUID::Parse("3f6b9c2a-1a7e-4c1d-9b2f").has_value());
}

RAVEN_TEST("uuid: nil is all-zero and parseable") {
    CHECK(UUID::Nil().IsNil());
    auto parsed = UUID::Parse("00000000-0000-0000-0000-000000000000");
    CHECK(parsed.has_value());
    CHECK(parsed->IsNil());
}

RAVEN_TEST("uuid: hash is stable within a run") {
    UUIDHash hash{};
    auto id = UUID::Generate();
    CHECK(hash(id) == hash(id));
}
