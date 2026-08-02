# Deliberately a stock JRE image with no agent baked in. The injector adds
# instrumentation at runtime through LD_PRELOAD, which is the property being
# demonstrated.
FROM eclipse-temurin:21-jdk AS build
WORKDIR /src
COPY java/Main.java .
RUN javac -d /out Main.java

FROM eclipse-temurin:21-jre
WORKDIR /app
COPY --from=build /out /app
CMD ["java", "-cp", "/app", "Main"]
