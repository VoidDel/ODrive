<template>
  <!-- render a list of choices horizontally in cards -->
  <div class="wizard-page card">
    <div class="wizard-title">{{title}}</div>
    <div class="wizard-choices">
      <!--
      <wizardChoice
        v-on:click.native="selectedChoice=choice.text;$emit('choice', {title: title, choice: choice.text, configStub: choice.configStub, hooks: choice.hooks})"
      />
      -->
      <template v-for="choice in choices">
        <component
          :class="{chosen: selectedChoice == choice.title, unchosen: selectedChoice != choice.title, inactive: !requirementsMet(choice.requirements, config)}"
          :is="choice.component"
          :data="choice.data"
          :config="config"
          :axis="axis"
          :title="translateChoice(choice.title)"
          :selected="selectedChoice == choice.title"
          :hooks="choice.hooks"
          :allowed="requirementsMet(choice.requirements, config)"
          v-tooltip.top="{
            content: requirementsMet(choice.requirements, config) ? translateTooltip(choice.tooltip) : translateTooltip(choice.altTooltip),
            class: 'tooltip-custom tooltip-other-custom fade-in',
            delay: 0,
            visible: choice.tooltip!=null,
          }"
          :key="choice.title"
          v-on:click.native="choiceHandler(choice)"
          v-on:choice="handleCustomChoice"
          v-on:undo-choice="undoChoice"
        />
      </template>
    </div>
    <div class="page-components">
      <template v-for="pageComponent in pageComponents">
        <component
          :is="pageComponent.component"
          :data="pageComponent.data"
          :key="pageComponent.id"
          :calibrating="calibrating"
          :calStatus="calStatus"
          v-on:page-comp-event="pageCompEvent"
        />
      </template>
    </div>
  </div>
</template>

<script>
import wizardChoice from "./choices/wizardChoice.vue";
import wizardMisc from "./choices/wizardMisc.vue";
import wizardMotor from "./choices/wizardMotor.vue";
import wizardEncoderIncremental from "./choices/wizardEncoderIncremental.vue";
import wizardEncoderIncrementalIndex from "./choices/wizardEncoderIncrementalIndex.vue";
import wizardEnd from "./choices/wizardEnd.vue";
import wizardMotorCal from "./page_components/wizardMotorCal.vue";
import wizardEncoderCal from "./page_components/wizardEncoderCal.vue";
import wizardCalStatus from "./page_components/wizardCalStatus.vue";
import wizardBrake from "./choices/wizardBrake.vue";
import wizardInputFiltered from "./choices/wizardInputFiltered.vue";
import wizardInputVelRamp from "./choices/wizardInputVelRamp.vue";
import wizardInputTrajectory from "./choices/wizardInputTrajectory.vue";

export default {
  name: "wizardPage",
  props: {
    choices: Array,
    title: String,
    customComponents: Array,
    axis: String,
    config: Object,
    pageComponents: Array,
    calibrating: Boolean,
    calStatus: Boolean,
  },
  components: {
    wizardChoice,
    wizardMisc,
    wizardMotor,
    wizardEncoderIncremental,
    wizardEncoderIncrementalIndex,
    wizardEnd,
    wizardMotorCal,
    wizardEncoderCal,
    wizardCalStatus,
    wizardBrake,
    wizardInputFiltered,
    wizardInputVelRamp,
    wizardInputTrajectory,
  },
  data: function () {
    return {
      selectedChoice: undefined,
    };
  },
  methods: {
    translateChoice(title) {
      // Map choice titles to translation keys
      const choiceMap = {
        "ODrive v3.6 24V": "wizard.choices.odrive24v",
        "ODrive v3.6 56V": "wizard.choices.odrive56v",
        "Brake Resistor": "wizard.choices.brakeResistor",
        "I don't have a brake resistor": "wizard.choices.noBrakeResistor",
        "ODrive D5065": "wizard.choices.d5065",
        "ODrive D6374": "wizard.choices.d6374",
        "Other Motor": "wizard.choices.otherMotor",
        "CUI AMT102V": "wizard.choices.cuiAmt102v",
        "Hall Effect": "wizard.choices.hallEffect",
        "Incremental Without Index": "wizard.choices.incrementalNoIndex",
        "Incremental With Index": "wizard.choices.incrementalWithIndex",
        "Position Control": "wizard.choices.positionControl",
        "Velocity Control": "wizard.choices.velocityControl",
        "Torque Control": "wizard.choices.torqueControl",
        "Voltage Control": "wizard.choices.voltageControl",
      };
      return choiceMap[title] ? this.$t(choiceMap[title]) : title;
    },
    translateTooltip(tooltip) {
      if (!tooltip) return tooltip;
      // Map tooltip content to translation keys
      const tooltipMap = {
        "When you slow down a motor, the energy has to go somewhere": "wizard.tooltips.brakeResistor",
        "Only operate without a brake resistor if your power source can handle reverse current": "wizard.tooltips.noBrake",
        "D5065 motor from the ODrive shop": "wizard.tooltips.d5065",
        "D6374 motor from the ODrive shop": "wizard.tooltips.d6374",
        "Bring your own motor!": "wizard.tooltips.otherMotor",
        "Incremental 8192cpr encoder from the ODrive shop": "wizard.tooltips.cuiEncoder",
        "If your motor is 'sensored' and you don't have another encoder": "wizard.tooltips.hallEffect",
        "Generic incremental encoder without index": "wizard.tooltips.incrementalNoIndex",
        "Generic incremental encoder with index": "wizard.tooltips.incrementalWithIndex",
        "Maintain a specific position": "wizard.tooltips.positionControl",
        "Maintain a specific speed": "wizard.tooltips.velocityControl",
        "Maintain a specific torque": "wizard.tooltips.torqueControl",
        "Direct voltage control": "wizard.tooltips.voltageControl",
        "Make a choice!": "wizard.tooltips.makeChoice",
      };

      // Check for partial matches
      for (const [key, value] of Object.entries(tooltipMap)) {
        if (tooltip.includes(key)) {
          return this.$t(value);
        }
      }
      return tooltip;
    },
    handleCustomChoice(e) {
      console.log(e);
      this.$emit("choice", e);
    },
    undoChoice(e) {
      this.$emit("undo-choice",e);
    },
    pageCompEvent(e) {
      this.$emit("page-comp-event", e);
    },
    requirementsMet(reqs, wizardConfig){
      let reqsMet = true;
      for (const fn of reqs) {
        reqsMet = reqsMet && fn(wizardConfig);
      }
      return reqsMet;
    },
    choiceHandler(choice) {
      console.log(choice.title);
      if (this.selectedChoice == choice.title) {
        // undo selection
        this.selectedChoice = undefined;
      }
      else if (this.requirementsMet(choice.requirements, this.config)){
        this.selectedChoice = choice.title;
      }
    }
  },
};
</script>

<style>
.wizard-page {
  display: flex;
  flex-direction: column;
  text-align: center;
}

.wizard-choices {
  display: flex;
  flex-direction: row;
  flex-wrap: wrap;
}

.page-components {
  display: flex;
  flex-direction: row;
  justify-content: center;
}

.chosen {
  border: 2px solid black;
}

.unchosen {
  border: 2px solid transparent;
}

.inactive {
  background-color: var(--bg-color);
}

.wizard-title {
  font-weight: bold;
}

.vue-tooltip.tooltip-custom {
  background-color: lightyellow; /* var(--fg-color); */
  border-radius: 0px;
  box-shadow: 0 4px 8px 0 rgba(0, 0, 0, 0.4);
  font-family: "Roboto", sans-serif;
  color: black;
}

.vue-tooltip.tooltip-custom .tooltip-arrow {
  display: none;
}

.vue-tooltip.fade-in {
  opacity: 0;
  animation: fadeIn ease 0.25s;
  animation-fill-mode: both;
}

@keyframes fadeIn {
  from {
    opacity: 0;
  }
  to {
    opacity: 1;
  }
}
</style>