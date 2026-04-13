// =============================================================================
// FIX Protocol Engine - Dictionary unit tests
// =============================================================================
#include <gtest/gtest.h>
#include "fix/dictionary/data_dictionary.hpp"
#include "fix/core/constants.hpp"
#include "fix/core/message.hpp"

using namespace fix;

TEST(DictionaryTest, BuiltinLoad42) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_2);

    EXPECT_EQ(d.version(), FixVersion::FIX_4_2);
    EXPECT_EQ(d.version_string(), "FIX.4.2");
}

TEST(DictionaryTest, FindFieldByTag) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    auto* f = d.find_field(tags::Symbol);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->tag, tags::Symbol);
    EXPECT_EQ(f->name, "Symbol");
}

TEST(DictionaryTest, FindFieldByName) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    auto* f = d.find_field("Symbol");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->tag, tags::Symbol);
}

TEST(DictionaryTest, UnknownFieldReturnsNull) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_2);

    EXPECT_EQ(d.find_field(99999), nullptr);
}

TEST(DictionaryTest, FindMessageType) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    auto* m = d.find_message("D");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->name, "NewOrderSingle");
    EXPECT_FALSE(m->is_admin);
}

TEST(DictionaryTest, FindAdminMessage) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    auto* m = d.find_message("A");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->name, "Logon");
    EXPECT_TRUE(m->is_admin);
}

TEST(DictionaryTest, ValidationPassesForValidMessage) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_2);

    Message m(msg_types::Logon);
    m.set(tags::EncryptMethod, "0");
    m.set(tags::HeartBtInt, "30");

    auto r = d.validate(m);
    EXPECT_TRUE(r.has_value());
}

TEST(DictionaryTest, ValidationFailsMissingRequired) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_2);

    Message m(msg_types::Logon);
    // Missing EncryptMethod and HeartBtInt
    auto r = d.validate(m);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::MissingField));
}

TEST(DictionaryTest, ValidationFailsInvalidEnumValue) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    Message m(msg_types::Logon);
    m.set(tags::EncryptMethod, "99"); // invalid enum
    m.set(tags::HeartBtInt, "30");

    auto r = d.validate(m);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::InvalidField));
}

TEST(DictionaryTest, CustomFieldRegistration) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    FieldDef custom;
    custom.tag  = 9999;
    custom.name = "MyCustomField";
    custom.type = FieldType::String;
    d.register_field(std::move(custom));

    auto* f = d.find_field(9999);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->name, "MyCustomField");
}

TEST(DictionaryTest, Reload) {
    DataDictionary d;
    d.load_builtin(FixVersion::FIX_4_4);

    // reload from a non-existent path should succeed (builtin is already loaded)
    auto r = d.reload("/nonexistent/path/FIX44.xml");
    EXPECT_TRUE(r.has_value());
}

TEST(DictionaryTest, ApplVerIDResolution) {
    EXPECT_EQ(DictionaryRegistry::resolve_appl_ver_id("9"), FixVersion::FIX_5_0SP2);
    EXPECT_EQ(DictionaryRegistry::resolve_appl_ver_id("8"), FixVersion::FIX_5_0SP1);
    EXPECT_EQ(DictionaryRegistry::resolve_appl_ver_id("6"), FixVersion::FIX_4_4);
    EXPECT_EQ(DictionaryRegistry::resolve_appl_ver_id("4"), FixVersion::FIX_4_2);
    EXPECT_EQ(DictionaryRegistry::resolve_appl_ver_id("?"), FixVersion::Unknown);
}

TEST(DictionaryTest, RegistryGetAndSet) {
    auto dict = std::make_shared<DataDictionary>();
    dict->load_builtin(FixVersion::FIX_5_0SP2);

    DictionaryRegistry::instance().set(FixVersion::FIX_5_0SP2, dict);

    const auto* d = DictionaryRegistry::instance().get(FixVersion::FIX_5_0SP2);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->version(), FixVersion::FIX_5_0SP2);
}
