load("@envoy//bazel:envoy_build_system.bzl", "envoy_mobile_package")
load("@envoy_mobile//bazel:envoy_mobile_android_test.bzl", "envoy_mobile_android_test")

licenses(["notice"])  # Apache 2

envoy_mobile_package()

envoy_mobile_android_test(
    name = "element_test",
    srcs = [
        "ElementTest.kt",
    ],
    deps = [
        "//library/kotlin/io/envoyproxy/envoymobile:envoy_interfaces_lib",
    ],
)

envoy_mobile_android_test(
    name = "tags_builder_test",
    srcs = [
        "TagsBuilderTest.kt",
    ],
    deps = [
        "//library/kotlin/io/envoyproxy/envoymobile:envoy_interfaces_lib",
    ],
)
