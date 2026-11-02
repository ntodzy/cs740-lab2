# Makefile adapted from
# http://make.mad-scientist.net/papers/advanced-auto-dependency-generation/

OBJDIR := .obj
DEPDIR := .d
DATADIR := data

$(shell mkdir -p $(OBJDIR) > /dev/null)
$(shell mkdir -p $(DEPDIR) > /dev/null)
$(shell mkdir -p $(DATADIR) > /dev/null)

CXX = clang++
CXXFLAGS = -std=c++11 -Wall -Wextra -g -Ofast
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td
POSTCOMPILE = @mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@

EXEC = htsim
SRCS = $(wildcard *.cpp load-balancers/*.cpp congestion/*.cpp packets/*.cpp stats/*.cpp)
OBJS = $(SRCS:%.cpp=$(OBJDIR)/%.o)

$(EXEC): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJS): $(OBJDIR)/%.o: %.cpp
$(OBJS): $(OBJDIR)/%.o: %.cpp $(DEPDIR)/%.d
	@mkdir -p $(dir $@) $(dir $(DEPDIR)/$*.Td)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@
	$(POSTCOMPILE)

$(DEPDIR)/%.d: ;

clean:
	rm -frv $(OBJDIR) $(DEPDIR) $(EXEC)

.PRECIOUS: $(DEPDIR)/%.d

.PHONY: clean

include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(SRCS))))
